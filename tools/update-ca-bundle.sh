#!/bin/sh
# Refresh the vendored CA bundle from curl's official Mozilla-derived source.
set -eu

OUT=${1:-cacert.pem}
TMP="${OUT}.tmp.$$"
trap 'rm -f "$TMP"' EXIT HUP INT TERM

curl --fail --location --proto '=https' --tlsv1.2 \
    --output "$TMP" https://curl.se/ca/cacert.pem
test -s "$TMP"
grep -q -- 'BEGIN CERTIFICATE' "$TMP"
mv "$TMP" "$OUT"
trap - EXIT HUP INT TERM
echo "Updated $OUT from https://curl.se/ca/cacert.pem"
