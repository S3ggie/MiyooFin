#!/bin/sh
# Remote screenshot capture for MiyooFin.
#
# Requests a framebuffer capture from the running MiyooFin app and pulls
# the resulting BMP over SSH.
#
# Usage:
#   tools/miyoo/miyoofin-screenshot.sh [output-path]
#
# The app must be running.  A flag file is touched on the device; the app
# captures its current framebuffer on the next frame and writes the BMP.
# This script polls for the file, pulls it, and prints its size.
# Override defaults with MIYOO_HOST and MIYOO_SSH_PORT environment variables.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$SCRIPT_DIR/ssh-common.sh"
TARGET=$MIYOO_SSH_TARGET

FLAG=/tmp/miyoofin-screenshot-request
REMOTE_BMP=/mnt/SDCARD/App/MiyooFin/screenshot.bmp
LOCAL_BMP=${1:-screenshot.bmp}
TIMEOUT=15

# Record the existing file's inode/mtime so we can detect a fresh capture.
BEFORE=$(miyoo_ssh "$TARGET" "stat -c '%i:%Y' '$REMOTE_BMP' 2>/dev/null" || true)

# Request the capture — touch the flag file; the app picks it up on the
# next frame (single stat(), no allocation when absent).
miyoo_ssh "$TARGET" "touch '$FLAG'" || {
    printf '[miyoofin-screenshot] ERROR: could not touch flag file\n' >&2
    exit 1
}

# Wait for the BMP to appear or refresh.
elapsed=0
while [ "$elapsed" -lt "$TIMEOUT" ]; do
    AFTER=$(miyoo_ssh "$TARGET" "stat -c '%i:%Y' '$REMOTE_BMP' 2>/dev/null" || true)
    if [ -n "$AFTER" ] && [ "$AFTER" != "$BEFORE" ]; then
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
if [ "$elapsed" -ge "$TIMEOUT" ]; then
    printf '[miyoofin-screenshot] ERROR: BMP did not appear within %ds\n' "$TIMEOUT" >&2
    exit 1
fi

# Pull the file.
miyoo_scp "$TARGET:$REMOTE_BMP" "$LOCAL_BMP" || {
    printf '[miyoofin-screenshot] ERROR: scp pull failed\n' >&2
    exit 1
}

# Print file info.
SIZE=$(stat -c '%s' "$LOCAL_BMP" 2>/dev/null || echo unknown)
# BMP dimensions are at offset 18 (4 bytes LE width) and 22 (4 bytes LE height).
if command -v xxd >/dev/null 2>&1; then
    W=$(xxd -l 4 -s 18 -p "$LOCAL_BMP" | sed 's/^0*//' | while read -r hex; do printf '%d' "0x${hex:-0}" 2>/dev/null || echo '?'; done)
    H=$(xxd -l 4 -s 22 -p "$LOCAL_BMP" | sed 's/^0*//' | while read -r hex; do printf '%d' "0x${hex:-0}" 2>/dev/null || echo '?'; done)
    printf '[miyoofin-screenshot] %s (%sx%s, %s bytes)\n' "$LOCAL_BMP" "${W:-?}" "${H:-?}" "$SIZE"
else
    printf '[miyoofin-screenshot] %s (%s bytes)\n' "$LOCAL_BMP" "$SIZE"
fi
