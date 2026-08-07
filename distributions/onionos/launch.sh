#!/bin/sh
# MiyooFin launch script for OnionOS
# Expected location: /mnt/SDCARD/Roms/PORTS/MiyooFin/launch.sh

# Change to the directory where this script resides
cd "$(dirname "$0")" || exit 1

# Signal OnionOS that we are running (keeps the device awake)
touch /tmp/stay_awake
trap 'rm -f /tmp/stay_awake' EXIT

# Bundle our own libraries so we don't depend on system versions
export LD_LIBRARY_PATH="${PWD}/lib:${LD_LIBRARY_PATH}"

# Force Miyoo-patched SDL video/audio drivers
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo

# Run the application
./miyoofin

exit 0