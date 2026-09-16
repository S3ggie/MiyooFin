#!/bin/sh
# Publish (or refresh) the private prebuilt Miyoo cross-toolchain image to GHCR.
#
# The image contains the Debian Buster arm-linux-gnueabihf cross-compiler plus
# the already-built Miyoo-patched SDL2, so CI can cross-build and verify the
# ARM binaries without the device-imported blobs in vendor/miyoo/lib. Those
# blobs are only needed to *build* SDL2 inside the image (see
# Dockerfile.onionos and docs/toolchain.md); pushing the finished image needs
# no blobs at all.
#
# Safe to re-run: re-tagging and re-pushing an unchanged image is a no-op
# server-side (same digest). No secrets live in this script or the repo:
# registry auth comes from $GHCR_TOKEN, from `gh auth token`, or from a
# `docker login ghcr.io` you performed beforehand (see --no-login below).
# Tokens are only ever piped via --password-stdin; they are never printed
# and never written to a file.
#
# Usage:
#   sh tools/push-toolchain-image.sh [-n|--dry-run] [--no-login] [owner]
#
#   owner defaults to $TOOLCHAIN_OWNER, else the owner of git remote `origin`
#   (lowercased for the GHCR path). The repository name is derived from the
#   same remote (default: MiyooFin).
#
# Auth precedence:
#   1. --no-login (or TOOLCHAIN_NO_LOGIN=1): skip this script's own
#      `docker login` entirely and use whatever credentials are already
#      stored (e.g. you ran `docker login ghcr.io -u <user> --password-stdin`
#      beforehand with a classic PAT).
#   2. $GHCR_TOKEN (a classic PAT with write:packages): used for
#      `docker login` via --password-stdin. Scopes cannot be introspected
#      for a PAT, so push errors are reported as they happen.
#   3. `gh auth token` (default): used for `docker login`, after a
#      fail-fast scope check (see check_gh_scopes).
#   4. Existing `docker login ghcr.io` credentials, if present.
set -eu

LOCAL_TAG="miyoofin-toolchain:latest"
IMAGE_NAME="miyoofin-toolchain"
TAG="latest"
DEFAULT_REPO="MiyooFin"

usage() {
    echo "Usage: $0 [-n|--dry-run] [--no-login] [owner]" >&2
    echo "  -n, --dry-run  print what would be done without pushing" >&2
    echo "  --no-login     skip 'docker login'; use existing credentials" >&2
    echo "                 (same as TOOLCHAIN_NO_LOGIN=1)" >&2
    echo "  owner          GHCR namespace (default: \$TOOLCHAIN_OWNER or git remote origin owner)" >&2
    echo "" >&2
    echo "Auth can also be supplied via env (see header for precedence):" >&2
    echo "  GHCR_TOKEN=<classic-PAT-with-write:packages>  (used via --password-stdin)" >&2
    echo "  GHCR_USER=<login user for GHCR_TOKEN>          (default: the owner, lowercased)" >&2
    echo "  TOOLCHAIN_NO_LOGIN=1  skip 'docker login'" >&2
    exit "${1:-1}"
}

DRY_RUN=0
NO_LOGIN=0
OWNER_ARG=""
case "${TOOLCHAIN_NO_LOGIN:-}" in
    1|[Tt][Rr][Uu][Ee]|[Yy][Ee][Ss]) NO_LOGIN=1 ;;
esac
for arg in "$@"; do
    case "$arg" in
        -n|--dry-run) DRY_RUN=1 ;;
        --no-login) NO_LOGIN=1 ;;
        -h|--help) usage 0 ;;
        -*) echo "ERROR: unknown flag: $arg" >&2; usage ;;
        *)
            if [ -z "$OWNER_ARG" ]; then
                OWNER_ARG="$arg"
            else
                echo "ERROR: unexpected argument: $arg" >&2; usage
            fi
            ;;
    esac
done

# --- Derive owner/repo from git remote origin (https or ssh forms) ---
REMOTE_URL=$(git remote get-url origin 2>/dev/null || true)
SLUG=""
case "$REMOTE_URL" in
    https://github.com/*) SLUG=${REMOTE_URL#https://github.com/} ;;
    http://github.com/*) SLUG=${REMOTE_URL#http://github.com/} ;;
    git@github.com:*) SLUG=${REMOTE_URL#git@github.com:} ;;
    ssh://git@github.com/*) SLUG=${REMOTE_URL#ssh://git@github.com/} ;;
esac
SLUG=${SLUG%.git}
REMOTE_OWNER=${SLUG%%/*}
REMOTE_REPO=${SLUG#*/}
[ "$REMOTE_OWNER" = "$SLUG" ] && REMOTE_OWNER=""
[ "$REMOTE_REPO" = "$SLUG" ] && REMOTE_REPO=""
[ -z "$REMOTE_OWNER" ] && REMOTE_OWNER=""
[ -z "$REMOTE_REPO" ] && REMOTE_REPO="$DEFAULT_REPO"

OWNER=${OWNER_ARG:-${TOOLCHAIN_OWNER:-$REMOTE_OWNER}}
if [ -z "$OWNER" ]; then
    echo "ERROR: cannot determine the image owner: no git remote 'origin' and no argument/env given." >&2
    echo "       Pass it explicitly: $0 <github-owner>" >&2
    exit 1
fi
OWNER_LC=$(printf '%s' "$OWNER" | tr '[:upper:]' '[:lower:]')
REPO=${REMOTE_REPO:-$DEFAULT_REPO}
SOURCE_URL="https://github.com/${OWNER}/${REPO}"
GHCR_REF="ghcr.io/${OWNER_LC}/${IMAGE_NAME}:${TAG}"

# --- The local image must already exist ---
if ! docker image inspect "$LOCAL_TAG" >/dev/null 2>&1; then
    echo "ERROR: local image '$LOCAL_TAG' not found." >&2
    echo "       Build it first (needs the device-imported blobs in vendor/miyoo/lib):" >&2
    echo "         make import-miyoo-libs" >&2
    echo "         docker build -f Dockerfile.onionos -t $LOCAL_TAG ." >&2
    echo "       See docs/toolchain.md for details, then re-run this script." >&2
    exit 1
fi

auth_remediation() {
    echo "       If the push is denied with a permission/auth error:" >&2
    echo "         - for a 'gh' login, refresh package scopes, then re-run:" >&2
    echo "             gh auth refresh -h github.com -s write:packages" >&2
    echo "           (if 'gh auth refresh' keeps returning the old scopes, the" >&2
    echo "           OAuth token cannot be upgraded: use a classic PAT instead.)" >&2
    echo "         - for a classic PAT (https://github.com/settings/tokens," >&2
    echo "           scope: write:packages), either export it as GHCR_TOKEN and" >&2
    echo "           re-run, or log in once and re-run with --no-login:" >&2
    echo "             docker login ghcr.io -u <github-user> --password-stdin" >&2
    echo "             sh tools/push-toolchain-image.sh --no-login [owner]" >&2
}

check_gh_scopes() {
    # Fail fast before a ~1GB push when GitHub reports the `gh` token
    # lacks package-write scopes. The token's scopes are visible in the
    # `x-oauth-scopes` response header of any API call. Fine-grained
    # tokens/PATs do not report that header, so an absent header (or an
    # introspection failure) is only a warning, not a fatal error.
    SCOPES_RAW=$(gh api -i user 2>/dev/null | grep -i '^x-oauth-scopes:' || true)
    if [ -z "$SCOPES_RAW" ]; then
        echo "WARNING: could not determine gh token scopes; continuing." >&2
        echo "         If the push is denied, it lacks 'write:packages'." >&2
        return 0
    fi
    case "$SCOPES_RAW" in
        *write:packages*|*admin:packages*) return 0 ;;
        *)
            echo "ERROR: your gh token lacks the 'write:packages' scope (scopes:$SCOPES_RAW)." >&2
            echo "       Pushing now would upload ~1GB just to be denied, so aborting." >&2
            auth_remediation
            exit 1
            ;;
    esac
}

ensure_auth() {
    # Log in to GHCR unless --no-login/TOOLCHAIN_NO_LOGIN=1 was given.
    # Every login step is status-checked; any failure aborts non-zero.
    if [ "$NO_LOGIN" = 1 ]; then
        echo "Skipping 'docker login' (--no-login): using existing credentials for ghcr.io."
        return 0
    fi
    if [ -n "${GHCR_TOKEN:-}" ]; then
        # A PAT's scopes cannot be introspected via x-oauth-scopes, so no
        # fail-fast check here: push errors are reported as they happen.
        GHCR_USER_LOGIN=${GHCR_USER:-$OWNER_LC}
        if ! printf '%s' "$GHCR_TOKEN" | docker login ghcr.io -u "$GHCR_USER_LOGIN" --password-stdin >/dev/null; then
            echo "ERROR: 'docker login ghcr.io' with \$GHCR_TOKEN failed." >&2
            exit 1
        fi
        echo "Logged in to ghcr.io as $GHCR_USER_LOGIN (via \$GHCR_TOKEN)."
        return 0
    fi
    if command -v gh >/dev/null 2>&1 && TOKEN=$(gh auth token -h github.com 2>/dev/null); then
        check_gh_scopes
        GH_USER=$(gh api user -q .login 2>/dev/null || printf '%s' "$OWNER_LC")
        if ! printf '%s' "$TOKEN" | docker login ghcr.io -u "$GH_USER" --password-stdin >/dev/null; then
            echo "ERROR: 'docker login ghcr.io' (via gh auth token) failed." >&2
            exit 1
        fi
        echo "Logged in to ghcr.io as $GH_USER (via gh auth token)."
    elif grep -qs 'ghcr.io' "${DOCKER_CONFIG:-$HOME/.docker}/config.json" 2>/dev/null; then
        echo "Using existing 'docker login' credentials for ghcr.io."
    else
        echo "ERROR: no container registry credentials found." >&2
        echo "       Log in first (classic PAT with 'write:packages'):" >&2
        echo "         docker login ghcr.io -u <github-user> --password-stdin" >&2
        echo "       then re-run with --no-login, or export GHCR_TOKEN, or run:" >&2
        echo "         gh auth refresh -h github.com -s write:packages" >&2
        exit 1
    fi
}

LABEL="org.opencontainers.image.source=${SOURCE_URL}"

if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] local image : $LOCAL_TAG (present)"
    echo "[dry-run] repo source : $SOURCE_URL"
    echo "[dry-run] label       : $LABEL"
    echo "[dry-run] target      : $GHCR_REF"
    if [ "$NO_LOGIN" = 1 ]; then
        echo "[dry-run] auth        : skip 'docker login' (--no-login); use existing credentials"
    elif [ -n "${GHCR_TOKEN:-}" ]; then
        echo "[dry-run] auth        : docker login ghcr.io (via \$GHCR_TOKEN, user: ${GHCR_USER:-$OWNER_LC})"
    else
        echo "[dry-run] would run   : docker login ghcr.io (via 'gh auth token' if available, else existing login)"
    fi
    echo "[dry-run] would run   : printf 'FROM $LOCAL_TAG\n' | docker build --label \"$LABEL\" -t \"$GHCR_REF\" -"
    echo "[dry-run] would run   : docker push \"$GHCR_REF\""
    echo "[dry-run] nothing pushed."
    exit 0
fi

ensure_auth

# Re-tag through a one-layer FROM build so the pushed image carries the
# source label. GitHub links the package to the repository from that label,
# which is what lets the workflow's GITHUB_TOKEN (packages: read) pull this
# private image. No rebuild of SDL2 happens here, so no device blobs needed.
if ! printf 'FROM %s\n' "$LOCAL_TAG" | docker build --label "$LABEL" -t "$GHCR_REF" -; then
    echo "ERROR: docker build of $GHCR_REF (re-tag with source label) failed." >&2
    exit 1
fi

# NOTE: POSIX sh has no `pipefail`, so `docker push ... | tee file` would
# report tee's status and mask a push failure (this script once claimed
# success on a denied push for exactly that reason). Capture to a file and
# check the push's own exit status instead.
OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT INT TERM
PUSH_STATUS=0
docker push "$GHCR_REF" >"$OUT" 2>&1 || PUSH_STATUS=$?
cat "$OUT"
if [ "$PUSH_STATUS" -ne 0 ]; then
    echo "ERROR: push of $GHCR_REF failed (docker push exited $PUSH_STATUS)." >&2
    auth_remediation
    exit 1
fi
if grep -Ei 'error from registry|permission_denied|[^a-z]denied[: ]|unauthorized|authentication required' "$OUT" >/dev/null 2>&1; then
    echo "ERROR: push of $GHCR_REF reported a registry error (see output above)." >&2
    echo "       Treating this as a failed push even though docker exited 0." >&2
    auth_remediation
    exit 1
fi
DIGEST=$(sed -n 's/.*digest: \(sha256:[0-9a-f][0-9a-f]*\).*/\1/p' "$OUT" | tail -n 1)
if [ -z "$DIGEST" ]; then
    DIGEST=$(docker inspect --format '{{index .RepoDigests 0}}' "$GHCR_REF" 2>/dev/null || true)
fi
echo "Pushed ${GHCR_REF}@${DIGEST:-<unknown digest>}"
