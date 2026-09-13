#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
RUNTIME_ROOT=${MIYOOFIN_DESKTOP_ROOT:-$ROOT/output/desktop-runtime}

fail() { echo "desktop runtime: $*" >&2; exit 1; }

[ -x "$ROOT/output/build/miyoofin" ] || fail "host binary missing; run 'make' first"
[ -x "$ROOT/output/build/miyoofin-https-bridge" ] || fail "host bridge missing; run 'make bridge' first"
command -v "${MIYOOFIN_FFPLAY_BIN:-ffplay}" >/dev/null 2>&1 \
    || fail "desktop FFplay missing; install ffmpeg or set MIYOOFIN_FFPLAY_BIN"

mkdir -p "$RUNTIME_ROOT"
cp "$ROOT/output/build/miyoofin" "$RUNTIME_ROOT/miyoofin"
cp "$ROOT/output/build/miyoofin-https-bridge" "$RUNTIME_ROOT/miyoofin-https-bridge"
cp "$ROOT/distributions/onionos/playback_runner.sh" "$RUNTIME_ROOT/playback_runner.sh"
if [ -x "$ROOT/output/build/miyoofin-playback-reporter" ]; then
    cp "$ROOT/output/build/miyoofin-playback-reporter" "$RUNTIME_ROOT/miyoofin-playback-reporter"
fi
if [ -s "$ROOT/cacert.pem" ]; then
    cp "$ROOT/cacert.pem" "$RUNTIME_ROOT/cacert.pem"
fi
chmod +x "$RUNTIME_ROOT/miyoofin" "$RUNTIME_ROOT/miyoofin-https-bridge" "$RUNTIME_ROOT/playback_runner.sh"

cd "$RUNTIME_ROOT"
unset SDL_VIDEODRIVER
unset SDL_AUDIODRIVER
export MIYOOFIN_DESKTOP_INPUT=1
export MIYOOFIN_DESKTOP_WINDOW=1
export MIYOOFIN_PLAYBACK_MODE=desktop
export MIYOOFIN_FFPLAY_BIN=${MIYOOFIN_FFPLAY_BIN:-ffplay}

echo "Starting MiyooFin desktop runtime in $RUNTIME_ROOT"
echo "Window size: 640x480"
echo "Use W/A/S/D for D-pad, Enter for A, Backspace for B, and click the top tabs to switch sections."
echo "Space/Escape remain accepted as desktop aliases; close the window to exit."
exec ./miyoofin "$@"
