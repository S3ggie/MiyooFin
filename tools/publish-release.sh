#!/bin/sh
# Publish a MiyooFin release to GitHub using the pre-built release assets.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

RELEASE_DIR="output/release"
RELEASE_ZIP="$RELEASE_DIR/MiyooFin.zip"
RELEASE_TAR="$RELEASE_DIR/MiyooFin.tar.gz"
RELEASE_MANIFEST="$RELEASE_DIR/manifest.json"
LEGAL_TEST="tests/test_release_legal.sh"
NOTES_FILE="${2:-}"

usage() {
    echo "Usage: $0 <tag> [notes-file]" >&2
    echo "  tag: release tag, e.g. v0.2.0" >&2
    echo "  notes-file: optional path to release notes (markdown)" >&2
    exit 1
}

[ $# -ge 1 ] || usage
TAG="$1"

# Validate tag format
case "$TAG" in
    v[0-9]*.*.*) ;;
    *)
        echo "ERROR: tag must start with 'v' followed by a semver (e.g. v0.2.0)" >&2
        exit 1
        ;;
esac

# --- Run legal tests first ---
[ -s "$LEGAL_TEST" ] || {
    echo "ERROR: legal test is missing or empty: $LEGAL_TEST" >&2
    exit 1
}
echo "Running legal tests before publish..."
sh "$LEGAL_TEST"

# --- Verify release assets exist ---
for f in "$RELEASE_ZIP" "$RELEASE_TAR" "$RELEASE_MANIFEST"; do
    [ -s "$f" ] || {
        echo "ERROR: release asset is missing or empty: $f" >&2
        echo "       Run tools/build-release.sh first." >&2
        exit 1
    }
done

# --- Verify manifest tag matches argument ---
MANIFEST_TAG=$(sed -n 's/.*"tag": *"\([^"]*\)".*/\1/p' "$RELEASE_MANIFEST" | head -1)
[ "$MANIFEST_TAG" = "$TAG" ] || {
    echo "ERROR: manifest tag '$MANIFEST_TAG' does not match argument '$TAG'" >&2
    exit 1
}

# --- Check tag does not already exist ---
if gh release view "$TAG" >/dev/null 2>&1; then
    echo "ERROR: release '$TAG' already exists on GitHub" >&2
    echo "       Delete or recreate it before publishing." >&2
    exit 1
fi

# --- Print sha256 checksums ---
echo ""
echo "Release assets:"
echo "  ZIP:     $(sha256sum "$RELEASE_ZIP" | cut -d' ' -f1)  $RELEASE_ZIP"
echo "  Tarball: $(sha256sum "$RELEASE_TAR" | cut -d' ' -f1)  $RELEASE_TAR"
echo "  Manifest: $(sha256sum "$RELEASE_MANIFEST" | cut -d' ' -f1)  $RELEASE_MANIFEST"
echo ""

# --- Prepare release notes ---
if [ -n "$NOTES_FILE" ] && [ -s "$NOTES_FILE" ]; then
    NOTES_FLAGS="--notes-file $NOTES_FILE"
else
    NOTES_FLAGS="--notes \"Release ${TAG}\""
fi

# --- Create GitHub release with assets ---
echo "Creating GitHub release $TAG..."
eval gh release create "$TAG" \
    "$RELEASE_ZIP" \
    "$RELEASE_TAR" \
    "$RELEASE_MANIFEST" \
    --title "MiyooFin $TAG" \
    $NOTES_FLAGS

echo ""
echo "Release $TAG published successfully."
echo "Manifest available at: https://github.com/S3ggie/MiyooFin/releases/latest/download/manifest.json"
