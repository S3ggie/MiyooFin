#!/bin/sh
# -------------------------------------------------------------------
# import-miyoo-build-libs.sh
#
# Import the six Miyoo build-time shared libraries from a Miyoo Mini /
# Mini Plus running OnionOS, reachable over SSH.
#
# All six files are fetched into a temporary directory first.  Only
# after every SHA-256 hash matches are the files installed via a
# transactional two-rename swap.  On failure the original directory
# is restored and the target directory is left unchanged.
#
# Usage:
#   tools/import-miyoo-build-libs.sh [--verify] [HOST]
#
# --verify   Only check SHA-256 of local files (no SSH needed).
# HOST       defaults to $MIYOO_HOST, then "miyoo".
# -------------------------------------------------------------------
set -eu

VERIFY_ONLY=0
HOST=""

for arg in "$@"; do
    case "$arg" in
        --verify) VERIFY_ONLY=1 ;;
        -*)       echo "Unknown option: $arg"; exit 1 ;;
        *)        HOST="$arg" ;;
    esac
done

if [ "$VERIFY_ONLY" -eq 0 ] && [ -z "$HOST" ]; then
    HOST="${MIYOO_HOST:-miyoo}"
fi

DEST_DIR="vendor/miyoo/lib"

# (remote path, local filename, expected sha256)
LIBS="/mnt/SDCARD/.tmp_update/lib/parasyte/libEGL.so.1       libEGL.so.1     c99c56ccf809b2aa81f4c69c074a3887229782e00dc78dd264846fda6db57f84
/mnt/SDCARD/.tmp_update/lib/parasyte/libGLESv2.so      libGLESv2.so    41c978f318170a134674db2136fe715d6f72de8b99bccdabdfe17716acfbddbb
/config/lib/libmi_ao.so                                libmi_ao.so     696a7270facf93987800ba7a340a4160ed11da0d7805d5b30bfac8ce44311212
/config/lib/libmi_common.so                            libmi_common.so 1685d354394190826e63aa8830c6f1328b6037caa657da912b74f10f19f8457f
/config/lib/libmi_gfx.so                               libmi_gfx.so    ecb2d75ce79afcc32691e6ca9da93ac7539a0bcde8463611e2a592acf66e6af1
/config/lib/libmi_sys.so                               libmi_sys.so    6e8ab2f6fd40c511f89d0f6fc29c8a57b462fe13cd5fc1a1a6b40ed1b89fee20"

# -------------------------------------------------------------------
# --verify mode: check SHA-256 of local files only
# -------------------------------------------------------------------
if [ "$VERIFY_ONLY" -ne 0 ]; then
    failed=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        set -- $line
        local_name="$2"
        expected_hash="$3"
        filepath="${DEST_DIR}/${local_name}"

        if [ ! -f "$filepath" ]; then
            echo "  MISSING  ${local_name}"
            failed=1
            continue
        fi

        actual_hash="$(sha256sum "$filepath" | cut -d' ' -f1)"
        if [ "$actual_hash" != "$expected_hash" ]; then
            echo "  FAIL     ${local_name}"
            echo "             expected ${expected_hash}"
            echo "             got      ${actual_hash}"
            failed=1
        else
            printf "  OK       %s\n" "$local_name"
        fi
    done <<ENDLIBS
$LIBS
ENDLIBS

    if [ "$failed" -ne 0 ]; then
        echo ""
        echo "ERROR: Miyoo build libraries are missing or invalid."
        echo "       Run 'make import-miyoo-libs' to re-import them from your Miyoo."
        exit 1
    fi
    echo "All six Miyoo build libraries verified."
    exit 0
fi

# -------------------------------------------------------------------
# Import mode: fetch from device and install transactionally
# -------------------------------------------------------------------
TMP_DIR="$(mktemp -d)"
cleanup() { rm -rf "$TMP_DIR"; }
trap cleanup EXIT

echo "Importing Miyoo build libraries from ${HOST}..."

# ---- Phase 1: Fetch into temp directory --------------------------------
while IFS= read -r line; do
    [ -z "$line" ] && continue
    set -- $line
    remote_path="$1"
    local_name="$2"
    expected_hash="$3"

    printf "  Fetching %s ... " "$local_name"
    if ! scp -q "${HOST}:${remote_path}" "${TMP_DIR}/${local_name}"; then
        echo "FAIL"
        echo ""
        echo "ERROR: Could not fetch ${local_name} from ${HOST}:${remote_path}"
        echo "       Is your Miyoo Mini / Mini Plus running, reachable over SSH,"
        echo "       and does it have the expected file at that path?"
        exit 1
    fi
    echo "OK"
done <<ENDLIBS
$LIBS
ENDLIBS

# ---- Phase 2: Verify all six SHA-256 hashes ----------------------------
echo ""
echo "Verifying SHA-256 hashes..."
failed=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    set -- $line
    local_name="$2"
    expected_hash="$3"

    actual_hash="$(sha256sum "${TMP_DIR}/${local_name}" | cut -d' ' -f1)"
    if [ "$actual_hash" != "$expected_hash" ]; then
        echo "  FAIL  ${local_name}"
        echo "        expected ${expected_hash}"
        echo "        got      ${actual_hash}"
        failed=1
    else
        printf "  OK    %s\n" "$local_name"
    fi
done <<ENDLIBS
$LIBS
ENDLIBS

if [ "$failed" -ne 0 ]; then
    echo ""
    echo "ERROR: One or more hashes did not match."
    echo "       No files were installed."
    echo "       The libraries in ${DEST_DIR}/ are unchanged."
    exit 1
fi

# ---- Phase 3: Transactional install via mv-based swap ------------------
# Build a fresh directory alongside the current one, then promote it
# into place.  If anything goes wrong the old copy stays and is
# restored on failure.  Individual renames are atomic on POSIX, but
# the two-rename sequence means a crash between the backup and
# promotion steps would leave the libraries in ${BAKE_DIR}.  The
# script detects and recovers from this on its next run.
echo ""

STAGE_DIR="${DEST_DIR}.new"
BAKE_DIR="${DEST_DIR}.bak"

# Recover from a previous interrupted swap.  If DEST_DIR is missing
# but BAKE_DIR exists, a prior run moved the original aside but
# crashed before promotion — restore it so the working tree is
# consistent before we proceed.
if [ ! -d "$DEST_DIR" ] && [ -d "$BAKE_DIR" ]; then
    echo "Restoring ${DEST_DIR}/ from previous interrupted swap backup..."
    mv "$BAKE_DIR" "$DEST_DIR"
elif [ -d "$BAKE_DIR" ]; then
    # DEST_DIR exists; BAKE_DIR is a stale leftover.  Discard it.
    rm -rf "$BAKE_DIR"
fi
# Remove any stale staging directory (should not normally exist here,
# but clear it defensively).
rm -rf "$STAGE_DIR"

mkdir -p "$STAGE_DIR"

while IFS= read -r line; do
    [ -z "$line" ] && continue
    set -- $line
    local_name="$2"
    cp "${TMP_DIR}/${local_name}" "${STAGE_DIR}/${local_name}"
done <<ENDLIBS
$LIBS
ENDLIBS

# Step 1: If an existing DEST_DIR is present, back it up.
# This MUST succeed; failure is fatal.
if [ -d "$DEST_DIR" ]; then
    if ! mv "$DEST_DIR" "$BAKE_DIR"; then
        echo "ERROR: Could not back up existing ${DEST_DIR}/ to ${BAKE_DIR}/."
        echo "       Check filesystem permissions and available space."
        rm -rf "$STAGE_DIR"
        exit 1
    fi
fi

# Step 2: Promote the staged directory into place.
if ! mv "$STAGE_DIR" "$DEST_DIR"; then
    # Roll back: restore the original directory from backup.
    mv "$BAKE_DIR" "$DEST_DIR" 2>/dev/null || true
    rm -rf "$STAGE_DIR"
    echo "ERROR: Failed to install libraries into ${DEST_DIR}/."
    echo "       The original files have been restored."
    exit 1
fi

# Step 3: Success — discard the backup.
rm -rf "$BAKE_DIR"
echo "All six libraries installed into ${DEST_DIR}/."
echo "You can now run:  make onionos"
