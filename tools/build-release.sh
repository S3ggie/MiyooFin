#!/bin/sh
# Build the redistributable MiyooFin release ZIP with required legal notices.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

make package

PACKAGE_DIR="output/package/MiyooFin"
RELEASE_DIR="output/release"
RELEASE_ZIP="$RELEASE_DIR/MiyooFin.zip"

[ -d "$PACKAGE_DIR" ] || {
    echo "ERROR: package directory was not created: $PACKAGE_DIR" >&2
    exit 1
}
[ -s LICENSE ] || {
    echo "ERROR: LICENSE is missing or empty" >&2
    exit 1
}
[ -s THIRD_PARTY_NOTICES.md ] || {
    echo "ERROR: THIRD_PARTY_NOTICES.md is missing or empty" >&2
    exit 1
}

cp LICENSE "$PACKAGE_DIR/LICENSE"
cp THIRD_PARTY_NOTICES.md "$PACKAGE_DIR/THIRD_PARTY_NOTICES.md"

[ -s "$PACKAGE_DIR/LICENSE" ] || {
    echo "ERROR: GPL license was not staged" >&2
    exit 1
}
[ -s "$PACKAGE_DIR/THIRD_PARTY_NOTICES.md" ] || {
    echo "ERROR: third-party notices were not staged" >&2
    exit 1
}

mkdir -p "$RELEASE_DIR"
rm -f "$RELEASE_ZIP"
(
    cd output/package
    zip -qr ../release/MiyooFin.zip MiyooFin
)

[ -s "$RELEASE_ZIP" ] || {
    echo "ERROR: release ZIP was not created" >&2
    exit 1
}

echo "Release package created: $RELEASE_ZIP"
echo "Included legal files: LICENSE, THIRD_PARTY_NOTICES.md"
