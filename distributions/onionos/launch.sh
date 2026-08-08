#!/bin/sh
# MiyooFin launch script for OnionOS
# Expected location: /mnt/SDCARD/App/MiyooFin/launch.sh
#
# Orchestrates:
#   1. MiyooFin normal startup
#   2. If playback-request.txt exists after exit:
#      a. Start miyoofin-https-bridge (HTTPS -> local HTTP)
#      b. Start Onion FFplay against http://127.0.0.1:18080/stream
#      c. Clean up bridge and playback request
#      d. Relaunch MiyooFin

APP_DIR="$(dirname "$0")"
cd "$APP_DIR" || exit 1

PLAYBACK_LOG="$APP_DIR/playback-launch.log"

playback_log() {
    echo "[$(date '+%H:%M:%S')] $1" >> "$PLAYBACK_LOG"
}

read_kv() {
    grep "^${2}=" "$1" 2>/dev/null | head -1 | cut -d'=' -f2-
}

cleanup_playback() {
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
    rm -f /tmp/stay_awake
}

# -------------------------------------------------------------------
# Main loop: run MiyooFin, and if a playback was requested, play
# then relaunch.
# -------------------------------------------------------------------
while true; do
    export LD_LIBRARY_PATH="${APP_DIR}/lib:${LD_LIBRARY_PATH}"
    export SDL_VIDEODRIVER=mmiyoo
    export SDL_AUDIODRIVER=mmiyoo

    ./miyoofin
    MIYOOFIN_EXIT=$?

    if [ "$MIYOOFIN_EXIT" -ne 42 ]; then
        exit 0
    fi

    if [ ! -f playback-request.txt ]; then
        playback_log "WARN: Exit code 42 but no playback-request.txt"
        exit 0
    fi

    playback_log "=== Playback request detected ==="

    REQUEST_ITEM_ID=$(read_kv playback-request.txt item_id)
    REQUEST_ITEM_TYPE=$(read_kv playback-request.txt item_type)

    if [ -z "$REQUEST_ITEM_ID" ]; then
        playback_log "ERROR: Missing item_id in playback-request.txt"
        rm -f playback-request.txt
        exit 0
    fi
    [ -z "$REQUEST_ITEM_TYPE" ] && REQUEST_ITEM_TYPE="movie"

    SERVER_URL=$(read_kv session.txt server_url)
    ACCESS_TOKEN=$(read_kv session.txt access_token)

    if [ -z "$SERVER_URL" ] || [ -z "$ACCESS_TOKEN" ]; then
        playback_log "ERROR: Missing server_url or access_token in session.txt"
        rm -f playback-request.txt
        exit 0
    fi

    for _f in miyoofin-https-bridge cacert.pem; do
        if [ ! -f "$_f" ]; then
            playback_log "ERROR: $_f not found"
            rm -f playback-request.txt
            exit 0
        fi
    done

    playback_log "Starting playback (item=${REQUEST_ITEM_ID}, type=${REQUEST_ITEM_TYPE})"
    # Construct the EXACT proven forced-transcode URL.
    # Never echo or log the URL (contains token).
    TURL="${SERVER_URL}/Videos/${REQUEST_ITEM_ID}/stream.ts"
    TURL="${TURL}?Static=false&VideoCodec=h264&AudioCodec=aac"
    TURL="${TURL}&MaxWidth=640&MaxHeight=480&MaxFramerate=30"
    TURL="${TURL}&MaxVideoBitDepth=8&VideoBitRate=1200000"
    TURL="${TURL}&AudioBitRate=96000&AudioChannels=2&MaxAudioChannels=2"
    TURL="${TURL}&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
    TURL="${TURL}&EnableAutoStreamCopy=false&Context=Streaming"
    TURL="${TURL}&SubtitleStreamIndex=-1&ApiKey=${ACCESS_TOKEN}"

    # Ensure clean state
    rm -f /tmp/stay_awake
    BRIDGE_PID=""
    trap 'cleanup_playback' EXIT

    # Keep the device awake during playback
    touch /tmp/stay_awake

    # Set CPU governor to performance (best-effort)
    echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null

    # Start the HTTPS bridge
    playback_log "Starting HTTPS bridge..."
    "$APP_DIR/miyoofin-https-bridge" \
        "$TURL" \
        "$APP_DIR/cacert.pem" \
        18080 \
        > "$APP_DIR/playback-bridge.log" 2>&1 &
    BRIDGE_PID=$!

    # Brief wait for bridge startup
    sleep 1

    if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
        playback_log "ERROR: Bridge failed to start (see playback-bridge.log)"
        cleanup_playback
        rm -f playback-request.txt
        trap - EXIT
        exit 0
    fi
    playback_log "Bridge running (PID=$BRIDGE_PID)"

    # Clear MiyooFin's SDL driver overrides so FFplay uses Onion's native drivers.
    unset SDL_VIDEODRIVER
    unset SDL_AUDIODRIVER

    # Run Onion FFplay
    SYS=/mnt/SDCARD/.tmp_update
    playback_log "Starting FFplay with Onion SDL environment"
    cd "$SYS" || {
        playback_log "ERROR: Cannot cd to $SYS"
        cleanup_playback
        rm -f playback-request.txt
        trap - EXIT
        exit 0
    }

    ./bin/ffplay \
        -autoexit \
        -vf "hflip,vflip" \
        -i "http://127.0.0.1:18080/stream" \
        > "$APP_DIR/playback-ffplay.log" 2>&1

    FFPLAY_EXIT=$?
    playback_log "FFplay exited with code $FFPLAY_EXIT"

    # Cleanup
    cleanup_playback
    rm -f playback-request.txt
    trap - EXIT

    playback_log "Returning to MiyooFin..."
    cd "$APP_DIR" || exit 1
done