#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_SCRIPT="$ROOT/tools/build-release.sh"
PUBLISH_SCRIPT="$ROOT/tools/publish-release.sh"
NOTICES="$ROOT/THIRD_PARTY_NOTICES.md"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

# --- Existing build script checks ---
[ -s "$BUILD_SCRIPT" ] || fail "tools/build-release.sh is missing or empty"
[ -s "$ROOT/LICENSE" ] || fail "LICENSE is missing or empty"
[ -s "$NOTICES" ] || fail "THIRD_PARTY_NOTICES.md is missing or empty"

grep -Fq 'cp LICENSE "$PACKAGE_DIR/LICENSE"' "$BUILD_SCRIPT" || \
    fail "release script does not stage LICENSE"
grep -Fq 'cp THIRD_PARTY_NOTICES.md "$PACKAGE_DIR/THIRD_PARTY_NOTICES.md"' "$BUILD_SCRIPT" || \
    fail "release script does not stage third-party notices"
grep -Fq 'zip -qr ../release/MiyooFin.zip MiyooFin' "$BUILD_SCRIPT" || \
    fail "release script does not create the distributable ZIP"

# --- New: build script must produce tarball and manifest ---
grep -Fq 'MiyooFin.tar.gz' "$BUILD_SCRIPT" || \
    fail "build script does not reference MiyooFin.tar.gz"
grep -Fq 'manifest.json' "$BUILD_SCRIPT" || \
    fail "build script does not reference manifest.json"
grep -Eq 'tar +-[a-zA-Z]*(cz|zc)' "$BUILD_SCRIPT" || \
    fail "build script does not create the gzipped tarball"
grep -Fq 'sha256sum' "$BUILD_SCRIPT" || \
    fail "build script does not compute sha256 checksums"
grep -Fq '"assets"' "$BUILD_SCRIPT" || \
    fail "build script does not generate manifest with assets field"
grep -Fq 'releases/download/' "$BUILD_SCRIPT" || \
    fail "build script manifest does not use GitHub releases download URL pattern"

# --- New: publish script exists and is executable ---
[ -s "$PUBLISH_SCRIPT" ] || fail "tools/publish-release.sh is missing or empty"
[ -x "$PUBLISH_SCRIPT" ] || fail "tools/publish-release.sh is not executable"
grep -Fq 'set -eu' "$PUBLISH_SCRIPT" || \
    fail "publish script does not use set -eu"
grep -Fq 'gh release create' "$PUBLISH_SCRIPT" || \
    fail "publish script does not call gh release create"
grep -Fq 'MiyooFin.zip' "$PUBLISH_SCRIPT" || \
    fail "publish script does not upload MiyooFin.zip"
grep -Fq 'MiyooFin.tar.gz' "$PUBLISH_SCRIPT" || \
    fail "publish script does not upload MiyooFin.tar.gz"
grep -Fq 'manifest.json' "$PUBLISH_SCRIPT" || \
    fail "publish script does not upload manifest.json"
grep -Fq 'test_release_legal.sh' "$PUBLISH_SCRIPT" || \
    fail "publish script does not run legal tests before publishing"

# --- Existing third-party notices checks ---
grep -Fq '478ddde6a415d48b4c497ce3a2679c08afd23f40' "$NOTICES" || \
    fail "SDL2 pinned source is not documented"
grep -Fq '8.3.0-6' "$NOTICES" || \
    fail "GCC runtime source version is not documented"
grep -Fq 'GCC RUNTIME LIBRARY EXCEPTION' "$NOTICES" || \
    fail "GCC Runtime Library Exception is not included"
grep -Fq 'Mozilla Public License' "$NOTICES" || \
    fail "Mozilla CA bundle license is not documented"

echo "Release legal-notice checks passed"
