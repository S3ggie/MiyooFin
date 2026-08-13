#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SCRIPT="$ROOT/tools/build-release.sh"
NOTICES="$ROOT/THIRD_PARTY_NOTICES.md"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

[ -s "$SCRIPT" ] || fail "tools/build-release.sh is missing or empty"
[ -s "$ROOT/LICENSE" ] || fail "LICENSE is missing or empty"
[ -s "$NOTICES" ] || fail "THIRD_PARTY_NOTICES.md is missing or empty"

grep -Fq 'cp LICENSE "$PACKAGE_DIR/LICENSE"' "$SCRIPT" || \
    fail "release script does not stage LICENSE"
grep -Fq 'cp THIRD_PARTY_NOTICES.md "$PACKAGE_DIR/THIRD_PARTY_NOTICES.md"' "$SCRIPT" || \
    fail "release script does not stage third-party notices"
grep -Fq 'zip -qr ../release/MiyooFin.zip MiyooFin' "$SCRIPT" || \
    fail "release script does not create the distributable ZIP"

grep -Fq '478ddde6a415d48b4c497ce3a2679c08afd23f40' "$NOTICES" || \
    fail "SDL2 pinned source is not documented"
grep -Fq '8.3.0-6' "$NOTICES" || \
    fail "GCC runtime source version is not documented"
grep -Fq 'GCC RUNTIME LIBRARY EXCEPTION' "$NOTICES" || \
    fail "GCC Runtime Library Exception is not included"
grep -Fq 'Mozilla Public License' "$NOTICES" || \
    fail "Mozilla CA bundle license is not documented"

echo "Release legal-notice checks passed"
