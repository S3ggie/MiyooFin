#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUNDLE="$ROOT/cacert.pem"

fail() { echo "CA bundle test failed: $*" >&2; exit 1; }

# The repository dependency is curl's official Mozilla-derived PEM bundle, not
# a device/user generated or provider-specific certificate.
[ -s "$BUNDLE" ] || fail 'cacert.pem is missing or empty'
grep -q -- 'BEGIN CERTIFICATE' "$BUNDLE" || fail 'cacert.pem is not PEM data'
grep -q -- 'Certificate data from Mozilla' "$BUNDLE" || fail 'cacert.pem is not curl/Mozilla data'
grep -q '^package: .*check-ca-bundle' "$ROOT/Makefile" || fail 'package does not require CA validation'
grep -q 'test -s $(PACKAGE_DIR)/cacert.pem' "$ROOT/Makefile" || fail 'package does not verify staged CA bundle'

# Secure playback must never opt out of libcurl certificate verification.
! rg -n 'CURLOPT_SSL_VERIFYPEER[[:space:]]*,[[:space:]]*0L|CURLOPT_SSL_VERIFYHOST[[:space:]]*,[[:space:]]*0L|--insecure|(^|[[:space:]])-k([[:space:]]|$)|--no-check-certificate' \
    "$ROOT/src" "$ROOT/tools" "$ROOT/distributions/onionos" || fail 'TLS verification bypass found'

echo '[test] curl/Mozilla CA bundle dependency and TLS policy OK'
