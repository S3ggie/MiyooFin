#!/bin/sh
# playback_runner.sh — In-process external playback runner for MiyooFin
#
# Spawned as a child process by the MiyooFin parent via fork/exec.
# The parent suspends SDL before forking and resumes after this script exits.
#
# Reads:
#   playback-request.txt  — item_id, item_type, and resume_ticks
#   session.txt           — canonical server_url, optional local/public routes, access_token
#
# Flow:
#   1. Parse item_id, server_url, access_token
#   2. Construct forced-transcode URL
#   3. Start miyoofin-https-bridge
#   4. Run Onion FFplay against http://127.0.0.1:18080/stream
#   5. Clean up and exit
#
# The parent process (MiyooFin) is blocked in waitpid() during playback.
# All screen state, ScreenStack, and UI state remain alive in the parent.

APP_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$APP_DIR" || exit 1

PLAYBACK_LOG="$APP_DIR/playback-launch.log"

# A result belongs only to the immediately preceding playback session.
# Clear it before any new-session validation can fail and return to the UI.
rm -f "$APP_DIR/playback-result.txt"

playback_log() {
    echo "[$(date '+%H:%M:%S')] $1" >> "$PLAYBACK_LOG"
}

read_kv() {
    grep "^${2}=" "$1" 2>/dev/null | head -1 | cut -d'=' -f2-
}

cleanup_playback() {
    if [ -n "$REPORTER_PID" ] && kill -0 "$REPORTER_PID" 2>/dev/null; then
        kill "$REPORTER_PID" 2>/dev/null
        wait "$REPORTER_PID" 2>/dev/null
    fi
    if [ -n "$BRIDGE_PID" ] && kill -0 "$BRIDGE_PID" 2>/dev/null; then
        kill "$BRIDGE_PID" 2>/dev/null
        wait "$BRIDGE_PID" 2>/dev/null
    fi
    rm -f /tmp/stay_awake
}

# -------------------------------------------------------------------
# Parse playback request
# -------------------------------------------------------------------
if [ ! -f playback-request.txt ]; then
    playback_log "ERROR: playback-request.txt not found"
    exit 1
fi

REQUEST_ITEM_ID=$(read_kv playback-request.txt item_id)
REQUEST_ITEM_TYPE=$(read_kv playback-request.txt item_type)
REQUEST_RESUME_TICKS=$(read_kv playback-request.txt resume_ticks)
REQUEST_SOURCE_MODE=$(read_kv playback-request.txt source_mode)
REQUEST_DOWNLOAD_SCOPE=$(read_kv playback-request.txt download_scope)
[ -z "$REQUEST_SOURCE_MODE" ] && REQUEST_SOURCE_MODE=jellyfin

# Keep ticks as a validated decimal string.  Shell arithmetic may not be
# 64-bit on the target device.
case "$REQUEST_RESUME_TICKS" in
    ''|*[!0-9]*) REQUEST_RESUME_TICKS=0 ;;
esac

if [ -z "$REQUEST_ITEM_ID" ]; then
    playback_log "ERROR: Missing item_id in playback-request.txt"
    rm -f playback-request.txt
    exit 1
fi
[ -z "$REQUEST_ITEM_TYPE" ] && REQUEST_ITEM_TYPE="movie"

case "$REQUEST_ITEM_ID" in *[!A-Za-z0-9_.-]*|'') playback_log "ERROR: Unsafe item id"; exit 1;; esac
case "$REQUEST_SOURCE_MODE" in jellyfin|local) ;; *) playback_log "ERROR: Invalid source mode"; exit 1;; esac
if [ "$REQUEST_SOURCE_MODE" = local ]; then
    case "$REQUEST_DOWNLOAD_SCOPE" in *[!A-Za-z0-9_.-]*|'') playback_log "ERROR: Unsafe download scope"; exit 1;; esac
fi

SERVER_URL=$(read_kv session.txt server_url)
LOCAL_SERVER_URL=$(read_kv session.txt local_server_url)
PUBLIC_SERVER_URL=$(read_kv session.txt public_server_url)
ACCESS_TOKEN=$(read_kv session.txt access_token)

if [ "$REQUEST_SOURCE_MODE" = jellyfin ] && { [ -z "$SERVER_URL" ] || [ -z "$ACCESS_TOKEN" ]; }; then
    playback_log "ERROR: Missing server_url or access_token in session.txt"
    rm -f playback-request.txt
    exit 1
fi

for _f in miyoofin-https-bridge; do
    if [ ! -f "$_f" ]; then
        playback_log "ERROR: $_f not found"
        rm -f playback-request.txt
        exit 1
    fi
done

playback_log "=== External playback request (item=${REQUEST_ITEM_ID}, type=${REQUEST_ITEM_TYPE}, source_mode=${REQUEST_SOURCE_MODE}) ==="

# -------------------------------------------------------------------
# Construct the EXACT proven forced-transcode URL.
# Never echo or log the URL (contains token).
# -------------------------------------------------------------------
build_stream_url() {
    _base=$1
    _url="${_base}/Videos/${REQUEST_ITEM_ID}/stream.ts"
    _url="${_url}?Static=false&VideoCodec=h264&AudioCodec=aac"
    _url="${_url}&MaxWidth=640&MaxHeight=480&MaxFramerate=30"
    _url="${_url}&MaxVideoBitDepth=8&VideoBitRate=1200000"
    _url="${_url}&AudioBitRate=96000&AudioChannels=2&MaxAudioChannels=2"
    _url="${_url}&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
    _url="${_url}&EnableAutoStreamCopy=false&Context=Streaming"
    _url="${_url}&SubtitleStreamIndex=-1&StartTimeTicks=${REQUEST_RESUME_TICKS}"
    _url="${_url}&PlaySessionId=${PLAY_SESSION_ID}&ApiKey=${ACCESS_TOKEN}"
    printf '%s' "$_url"
}
PLAY_SESSION_ID="miyoofin-$(date +%s)-$$"
PUBLIC_BASE=${PUBLIC_SERVER_URL:-$SERVER_URL}
LAN_BASE=$LOCAL_SERVER_URL
# public_server_url is only set for LAN-canonical sessions.
if [ -z "$LAN_BASE" ] && [ -n "$PUBLIC_SERVER_URL" ]; then LAN_BASE=$SERVER_URL; fi
PUBLIC_TURL=$(build_stream_url "$PUBLIC_BASE")
TURL=$PUBLIC_TURL
FALLBACK_TURL=""
if [ "$REQUEST_SOURCE_MODE" = jellyfin ] && [ -n "$LAN_BASE" ]; then
    TURL=$(build_stream_url "$LAN_BASE")
    FALLBACK_TURL=$PUBLIC_TURL
    playback_log "[PlaybackRoute] LAN"
elif [ "$REQUEST_SOURCE_MODE" = jellyfin ]; then
    playback_log "[PlaybackRoute] PUBLIC"
fi

# The bridge accepts an empty CA path for HTTP-only routes.  Keep TLS
# verification mandatory whenever an HTTPS route is used, but do not prevent a
# usable LAN HTTP route from starting merely because its optional HTTPS fallback
# cannot be verified on this device.
CA_CERT_PATH=""
if [ "$REQUEST_SOURCE_MODE" = jellyfin ]; then
    case "$TURL" in https://*) PRIMARY_USES_HTTPS=1 ;; *) PRIMARY_USES_HTTPS=0 ;; esac
    case "$FALLBACK_TURL" in https://*) FALLBACK_USES_HTTPS=1 ;; *) FALLBACK_USES_HTTPS=0 ;; esac
    if [ "$PRIMARY_USES_HTTPS" = 1 ] || [ "$FALLBACK_USES_HTTPS" = 1 ]; then
        if [ -s cacert.pem ]; then
            CA_CERT_PATH="$APP_DIR/cacert.pem"
        elif [ "$PRIMARY_USES_HTTPS" = 1 ]; then
            playback_log "ERROR: cacert.pem not found for HTTPS playback"
            exit 1
        else
            playback_log "WARNING: Secure HTTPS fallback unavailable: cacert.pem not found; using LAN HTTP only"
            FALLBACK_TURL=""
        fi
    fi
fi
playback_log "Resume ticks=$REQUEST_RESUME_TICKS PlaySessionId=$PLAY_SESSION_ID"

# -------------------------------------------------------------------
# Ensure clean state
# -------------------------------------------------------------------
rm -f /tmp/stay_awake
BRIDGE_PID=""
REPORTER_PID=""
trap 'cleanup_playback' EXIT

# Keep the device awake during playback
touch /tmp/stay_awake

# Set CPU governor to performance (best-effort)
echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null

# -------------------------------------------------------------------
# Start the HTTPS bridge
# -------------------------------------------------------------------
playback_log "Starting media bridge..."
if [ "$REQUEST_SOURCE_MODE" = local ]; then
    LOCAL_MANIFEST="$APP_DIR/downloads/$REQUEST_DOWNLOAD_SCOPE/items/$REQUEST_ITEM_ID/manifest.v2"
    [ -f "$LOCAL_MANIFEST" ] || { playback_log "ERROR: Local manifest missing"; exit 1; }
    "$APP_DIR/miyoofin-https-bridge" --local-manifest "$LOCAL_MANIFEST" 18080 \
    > "$APP_DIR/playback-bridge.log" 2>&1 &
else
    if [ -n "$FALLBACK_TURL" ]; then
        "$APP_DIR/miyoofin-https-bridge" --fallback-url "$FALLBACK_TURL" "$TURL" "$CA_CERT_PATH" 18080 \
        > "$APP_DIR/playback-bridge.log" 2>&1 &
    else
        "$APP_DIR/miyoofin-https-bridge" "$TURL" "$CA_CERT_PATH" 18080 \
    > "$APP_DIR/playback-bridge.log" 2>&1 &
    fi
fi
BRIDGE_PID=$!

if [ "$REQUEST_SOURCE_MODE" = local ]; then
    PLAY_URL="http://127.0.0.1:18080/local.m3u8"
else
    PLAY_URL="http://127.0.0.1:18080/stream"
fi

# Brief wait for bridge startup
sleep 1

if ! kill -0 "$BRIDGE_PID" 2>/dev/null; then
    playback_log "ERROR: Bridge failed to start (see playback-bridge.log)"
    cleanup_playback
    rm -f playback-request.txt
    trap - EXIT
    exit 1
fi
playback_log "Bridge running (PID=$BRIDGE_PID)"

# -------------------------------------------------------------------
# Start the playback reporter (background, decoupled from FFplay I/O)
# -------------------------------------------------------------------
if [ -f "$APP_DIR/miyoofin-playback-reporter" ]; then
    playback_log "Starting playback reporter..."
    rm -f "$APP_DIR/playback-ffplay-exit.txt"
    rm -f "$APP_DIR/playback-reporter.log"
    # Create/truncate the log file BEFORE the reporter opens it.
    # FFplay will append (>>) below, so the reporter sees a stable file.
    : > "$APP_DIR/playback-ffplay.log"
    "$APP_DIR/miyoofin-playback-reporter" "$APP_DIR" &
    REPORTER_PID=$!
    playback_log "Reporter running (PID=$REPORTER_PID)"
else
    playback_log "WARNING: miyoofin-playback-reporter not found, skipping reporting"
fi

# -------------------------------------------------------------------
# Clear MiyooFin's SDL driver overrides so FFplay uses Onion's native
# drivers.
# -------------------------------------------------------------------
unset SDL_VIDEODRIVER
unset SDL_AUDIODRIVER

# -------------------------------------------------------------------
# Run Onion FFplay
# -------------------------------------------------------------------
SYS=/mnt/SDCARD/.tmp_update
playback_log "Starting FFplay with Onion SDL environment"
cd "$SYS" || {
    playback_log "ERROR: Cannot cd to $SYS"
    cleanup_playback
    rm -f playback-request.txt
    trap - EXIT
    exit 1
}

./bin/ffplay \
    -stats \
    -autoexit \
    -vf "hflip,vflip,split=2[main][tap];[tap]select=isnan(prev_selected_t)+gte(t-prev_selected_t\,5)+lte(t-prev_selected_t\,-5),showinfo,nullsink;[main]null" \
    -i "$PLAY_URL" \
    >> "$APP_DIR/playback-ffplay.log" 2>&1

FFPLAY_EXIT=$?
playback_log "FFplay exited with code $FFPLAY_EXIT"

# -------------------------------------------------------------------
# Signal FFplay exit to reporter
# -------------------------------------------------------------------
if [ -n "$REPORTER_PID" ]; then
    printf '%s' "$FFPLAY_EXIT" > "$APP_DIR/playback-ffplay-exit.txt"

    # Wait for reporter to send stopped report and exit (bounded)
    REPORTER_WAIT=0
    while kill -0 "$REPORTER_PID" 2>/dev/null && [ "$REPORTER_WAIT" -lt 8 ]; do
        sleep 1
        REPORTER_WAIT=$((REPORTER_WAIT + 1))
    done
    if kill -0 "$REPORTER_PID" 2>/dev/null; then
        playback_log "WARNING: Reporter did not exit within timeout, terminating"
        kill "$REPORTER_PID" 2>/dev/null
        wait "$REPORTER_PID" 2>/dev/null
    else
        playback_log "Reporter exited cleanly"
    fi
fi

# -------------------------------------------------------------------
# Cleanup
# -------------------------------------------------------------------
cleanup_playback
rm -f playback-request.txt
rm -f "$APP_DIR/playback-ffplay-exit.txt"
trap - EXIT

playback_log "=== Playback complete, returning to MiyooFin ==="
cd "$APP_DIR" || exit 0
exit 0
