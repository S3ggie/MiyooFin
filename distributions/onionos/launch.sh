#!/bin/sh
# MiyooFin launch script for OnionOS
# Expected location: /mnt/SDCARD/App/MiyooFin/launch.sh
#
# Simple launcher — all playback orchestration is handled in-process
# by MiyooFin itself (B5f3a).

APP_DIR="$(dirname "$0")"
cd "$APP_DIR" || exit 1

export LD_LIBRARY_PATH="${APP_DIR}/lib:${LD_LIBRARY_PATH}"
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo

./miyoofin
exit $?
