#!/bin/sh
# Build the redistributable MiyooFin release ZIP, tarball, and OTA manifest.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

make package

PACKAGE_DIR="output/package/MiyooFin"
RELEASE_DIR="output/release"
RELEASE_ZIP="$RELEASE_DIR/MiyooFin.zip"
RELEASE_TAR="$RELEASE_DIR/MiyooFin.tar.gz"
RELEASE_MANIFEST="$RELEASE_DIR/manifest.json"

# --- Derive version from include/miyoofin/version.hpp ---
VERSION_HPP="include/miyoofin/version.hpp"
[ -s "$VERSION_HPP" ] || {
    echo "ERROR: version header is missing or empty: $VERSION_HPP" >&2
    exit 1
}
VERSION_STR=$(sed -n 's/.*VERSION_STR *= *"\([^"]*\)".*/\1/p' "$VERSION_HPP" | head -1)
case "$VERSION_STR" in
    v*) VERSION_STR="${VERSION_STR#v}" ;;
esac
case "$VERSION_STR" in
    [0-9]*.*.*) ;;
    *)
        echo "ERROR: could not parse VERSION_STR from $VERSION_HPP (got: '$VERSION_STR')" >&2
        exit 1
        ;;
esac

TAG="v${VERSION_STR}"

# --- Existing pre-flight checks ---
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

# --- Create ZIP (existing behavior) ---
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

# --- Create gzipped tarball ---
rm -f "$RELEASE_TAR"
# Dereference symlinks so the OTA archive contains no symlink entries.
# The bundled lib/*.so are symlinks; the installer rejects symlink/hardlink
# entries (tar-slip defence), so store the real files instead.
tar -czhpf "$RELEASE_TAR" -C output/package MiyooFin

[ -s "$RELEASE_TAR" ] || {
    echo "ERROR: release tarball was not created" >&2
    exit 1
}

# --- Compute hashes and sizes ---
ZIP_SHA256=$(sha256sum "$RELEASE_ZIP" | cut -d' ' -f1)
ZIP_SIZE=$(stat -c %s "$RELEASE_ZIP")
TAR_SHA256=$(sha256sum "$RELEASE_TAR" | cut -d' ' -f1)
TAR_SIZE=$(stat -c %s "$RELEASE_TAR")

# --- Generate OTA manifest ---
published_at=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

cat > "$RELEASE_MANIFEST" <<EOF
{
  "name": "MiyooFin",
  "version": "${VERSION_STR}",
  "tag": "${TAG}",
  "min_version": "0.1.0",
  "published_at": "${published_at}",
  "notes": "Release ${TAG}",
  "assets": {
    "tar_gz": {
      "url": "https://github.com/S3ggie/MiyooFin/releases/download/${TAG}/MiyooFin.tar.gz",
      "sha256": "${TAR_SHA256}",
      "size": ${TAR_SIZE}
    },
    "zip": {
      "url": "https://github.com/S3ggie/MiyooFin/releases/download/${TAG}/MiyooFin.zip",
      "sha256": "${ZIP_SHA256}",
      "size": ${ZIP_SIZE}
    }
  }
}
EOF

[ -s "$RELEASE_MANIFEST" ] || {
    echo "ERROR: OTA manifest was not created" >&2
    exit 1
}

echo "Release package created: $RELEASE_ZIP"
echo "Release tarball created: $RELEASE_TAR"
echo "OTA manifest created: $RELEASE_MANIFEST"
echo "Version: ${TAG}"
echo "Included legal files: LICENSE, THIRD_PARTY_NOTICES.md"
