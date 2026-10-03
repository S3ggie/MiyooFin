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

stop_and_reap_child() {
    CHILD_NAME=$1
    CHILD_PID=$2
    [ -n "$CHILD_PID" ] || return

    if kill -0 "$CHILD_PID" 2>/dev/null; then
        playback_log "terminating $CHILD_NAME pid=$CHILD_PID"
        kill -TERM "$CHILD_PID" 2>/dev/null
        # Every PID here is an exact child of this runner.  Give it a brief,
        # bounded graceful window, then escalate only for that known PID.
        CHILD_STOP_WAIT=0
        while kill -0 "$CHILD_PID" 2>/dev/null && [ "$CHILD_STOP_WAIT" -lt 2 ]; do
            sleep 1
            CHILD_STOP_WAIT=$((CHILD_STOP_WAIT + 1))
        done
        if kill -0 "$CHILD_PID" 2>/dev/null; then
            playback_log "$CHILD_NAME did not exit after TERM; sending KILL pid=$CHILD_PID"
            kill -KILL "$CHILD_PID" 2>/dev/null
        fi
    fi
    wait "$CHILD_PID" 2>/dev/null || true
    playback_log "${CHILD_NAME}_reaped pid=$CHILD_PID"
}

cleanup_playback() {
    if [ -n "$FFPLAY_PID" ]; then
        stop_and_reap_child ffplay "$FFPLAY_PID"
        FFPLAY_PID=""
    fi
    if [ -n "$SUBS_PID" ]; then
        stop_and_reap_child subs-fetch "$SUBS_PID"
        SUBS_PID=""
    fi
    if [ -n "$REPORTER_PID" ]; then
        stop_and_reap_child reporter "$REPORTER_PID"
        REPORTER_PID=""
    fi
    if [ -n "$BRIDGE_PID" ]; then
        stop_and_reap_child bridge "$BRIDGE_PID"
        BRIDGE_PID=""
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
REQUEST_DURATION_TICKS=$(read_kv playback-request.txt duration_ticks)
REQUEST_NEXT_ID=$(read_kv playback-request.txt next_item_id)
REQUEST_NEXT_DURATION=$(read_kv playback-request.txt next_duration_ticks)
case "$REQUEST_NEXT_ID" in *[!A-Za-z0-9_.-]*) REQUEST_NEXT_ID="" ;; esac
case "$REQUEST_NEXT_DURATION" in ''|*[!0-9]*) REQUEST_NEXT_DURATION=0 ;; esac
REQUEST_SOURCE_MODE=$(read_kv playback-request.txt source_mode)
REQUEST_DOWNLOAD_SCOPE=$(read_kv playback-request.txt download_scope)
[ -z "$REQUEST_SOURCE_MODE" ] && REQUEST_SOURCE_MODE=jellyfin

case "$REQUEST_DURATION_TICKS" in
    ''|*[!0-9]*) REQUEST_DURATION_TICKS=0 ;;
esac

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
SESSION_ROUTES=$(read_kv session.txt routes)
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
# Remote playback only: ask the server where playback really is.  The screen
# wrote resume_ticks from its CACHED item, which is stale after the item is
# watched on another device; starting there would resume in the wrong place
# and later report that stale base back over the newer progress.  The reporter
# rewrites resume_ticks in playback-request.txt.  Best effort and bounded: the
# reporter has its own short HTTP timeouts and this watchdog stops it (TERM,
# then KILL) if it ever hangs, so a slow or unreachable server never blocks
# playback; every failure keeps the cached position.  Downloaded (local)
# playback reconciles through the offline journal instead.
# -------------------------------------------------------------------
refresh_resume_from_server() {
    [ -f "$APP_DIR/miyoofin-playback-reporter" ] || return 0
    _rr_limit=${MIYOOFIN_RESUME_REFRESH_TIMEOUT_S:-8}
    case "$_rr_limit" in ''|*[!0-9]*) _rr_limit=8 ;; esac
    _rr_out="$APP_DIR/playback-resume-refresh.txt"
    rm -f "$_rr_out"
    "$APP_DIR/miyoofin-playback-reporter" "$APP_DIR" --refresh-resume > "$_rr_out" 2>/dev/null &
    _rr_pid=$!
    (
        sleep "$_rr_limit"
        kill -TERM "$_rr_pid" 2>/dev/null || exit 0
        sleep 2
        kill -KILL "$_rr_pid" 2>/dev/null
    ) >/dev/null 2>&1 &
    _rr_guard=$!
    wait "$_rr_pid" 2>/dev/null || true
    kill "$_rr_guard" 2>/dev/null || true
    wait "$_rr_guard" 2>/dev/null || true
    if [ -s "$_rr_out" ]; then
        playback_log "$(head -1 "$_rr_out")"
    else
        playback_log "resume_refresh no result (kept cached ticks)"
    fi
    rm -f "$_rr_out"
    REQUEST_RESUME_TICKS=$(read_kv playback-request.txt resume_ticks)
    case "$REQUEST_RESUME_TICKS" in
        ''|*[!0-9]*) REQUEST_RESUME_TICKS=0 ;;
    esac
}
if [ "$REQUEST_SOURCE_MODE" = jellyfin ]; then
    refresh_resume_from_server
fi

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
if [ "$SESSION_ROUTES" = 2 ]; then
    # The two addresses exactly as the user set them (either may be empty); server_url is
    # only the identity caches are filed under.
    PUBLIC_BASE=$PUBLIC_SERVER_URL
    LAN_BASE=$LOCAL_SERVER_URL
    if [ -z "$PUBLIC_BASE" ] && [ -z "$LAN_BASE" ]; then PUBLIC_BASE=$SERVER_URL; fi
else
    PUBLIC_BASE=${PUBLIC_SERVER_URL:-$SERVER_URL}
    LAN_BASE=$LOCAL_SERVER_URL
    # public_server_url is only set for LAN-canonical sessions.
    if [ -z "$LAN_BASE" ] && [ -n "$PUBLIC_SERVER_URL" ]; then LAN_BASE=$SERVER_URL; fi
fi
PUBLIC_TURL=""
[ -n "$PUBLIC_BASE" ] && PUBLIC_TURL=$(build_stream_url "$PUBLIC_BASE")
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
SUBS_PID=""
FFPLAY_PID=""
trap 'cleanup_playback' EXIT
trap 'exit 143' HUP INT TERM

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
# Select the player environment.  The packaged OnionOS path retains its
# device-specific SDL/audio setup.  The host development path is explicit so
# it never inherits Onion driver names or an ARM-only audio preload.
# -------------------------------------------------------------------
PLAYBACK_MODE=${MIYOOFIN_PLAYBACK_MODE:-onion}
case "$PLAYBACK_MODE" in
    onion)
        unset SDL_VIDEODRIVER
        unset SDL_AUDIODRIVER
        PLAYBACK_FFPLAY_BIN=/mnt/SDCARD/.tmp_update/bin/ffplay
        PLAYBACK_FFPLAY_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so
        playback_log "player_env mode=onion SDL_VIDEODRIVER=(native) SDL_AUDIODRIVER=(native) LD_PRELOAD=$PLAYBACK_FFPLAY_PRELOAD"
        ;;
    desktop)
        unset SDL_VIDEODRIVER
        unset SDL_AUDIODRIVER
        PLAYBACK_FFPLAY_BIN=${MIYOOFIN_FFPLAY_BIN:-ffplay}
        PLAYBACK_FFPLAY_PRELOAD=
        playback_log "player_env mode=desktop SDL_VIDEODRIVER=(default) SDL_AUDIODRIVER=(default) FFPLAY=$PLAYBACK_FFPLAY_BIN"
        ;;
    *)
        playback_log "ERROR: Invalid playback mode"
        cleanup_playback
        rm -f playback-request.txt
        trap - EXIT
        exit 1
        ;;
esac

# -------------------------------------------------------------------
# Run FFplay
# -------------------------------------------------------------------
if [ "$PLAYBACK_MODE" = onion ]; then
    SYS=/mnt/SDCARD/.tmp_update
    playback_log "Starting FFplay with Onion SDL environment"
else
    SYS=
    playback_log "Starting FFplay with desktop SDL environment"
fi
# The bridge URL is intentionally redacted: keep argv shape without ever
# persisting a possibly authenticated input URL.
playback_log "ffplay_argv=-stats -autoexit -fs -vf=<redacted-filter> -i=<redacted-url>"
if [ "$PLAYBACK_MODE" = onion ]; then
    cd "$SYS" || {
        playback_log "ERROR: Cannot cd to $SYS"
        cleanup_playback
        rm -f playback-request.txt
        trap - EXIT
        exit 1
    }
    # miyoofin-player is our fork of this same ffplay (same libraries, same
    # arguments).  Stock ffplay stays the fallback: used when the fork is
    # missing, when MIYOOFIN_PLAYER=stock, or when the fork fails to start.
    PLAYER_KIND=stock
    PLAYER_BIN=./bin/ffplay
    PLAYER_EXTRA_ARGS=
    if [ "${MIYOOFIN_PLAYER:-fork}" != stock ] && [ -x "$APP_DIR/miyoofin-player" ]; then
        PLAYER_KIND=fork
        PLAYER_BIN="$APP_DIR/miyoofin-player"
        # The vflip,hflip filter below means the viewer sees the picture
        # rotated; the fork draws its on-screen display pre-rotated to match.
        PLAYER_EXTRA_ARGS="-osd_rot180 -osd_screen_rot180 -prefs $APP_DIR/player-prefs.txt"
        # The transcode is always H.264 + AAC in an MPEG-TS stream: a short probe
        # finds both streams, so every (re)open reaches the first picture sooner.
        if [ "$REQUEST_SOURCE_MODE" = jellyfin ]; then
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -probesize 262144 -analyzeduration 500000"
        fi
        # Library runtime (informational): the local HLS playlist cannot report
        # a trustworthy duration, so the on-screen progress bar uses this.
        # Stream time 0 is the resume offset for remote playback (StartTimeTicks).
        if [ "$REQUEST_SOURCE_MODE" = jellyfin ] && [ "${#REQUEST_RESUME_TICKS}" -gt 7 ]; then
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -osd_base $(printf '%s' "$REQUEST_RESUME_TICKS" | sed 's/.\{7\}$//')"
        fi
        # Subtitles. Remote playback fetches the track list and text subtitles in
        # the background (best effort, bounded by cleanup); downloaded playback
        # reads the copies saved beside the download, topping them up from the
        # server when they are missing and it is reachable.  The player picks
        # files up as they appear.
        if [ "$REQUEST_SOURCE_MODE" = jellyfin ] && [ -x "$APP_DIR/miyoofin-playback-reporter" ]; then
            rm -rf "$APP_DIR/subs" "$APP_DIR/playback-tracks.txt" "$APP_DIR/playback-segments.txt"
            "$APP_DIR/miyoofin-playback-reporter" "$APP_DIR" --fetch-subs > "$APP_DIR/playback-subs.log" 2>&1 &
            SUBS_PID=$!
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -subs_dir $APP_DIR -segments $APP_DIR/playback-segments.txt"
        elif [ "$REQUEST_SOURCE_MODE" = local ]; then
            LOCAL_ITEM_DIR="$APP_DIR/downloads/$REQUEST_DOWNLOAD_SCOPE/items/$REQUEST_ITEM_ID"
            if [ ! -f "$LOCAL_ITEM_DIR/playback-tracks.txt" ] && [ -x "$APP_DIR/miyoofin-playback-reporter" ]; then
                "$APP_DIR/miyoofin-playback-reporter" "$APP_DIR" --fetch-subs > "$APP_DIR/playback-subs.log" 2>&1 &
                SUBS_PID=$!
            fi
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -subs_dir $LOCAL_ITEM_DIR -osd_local -segments $LOCAL_ITEM_DIR/playback-segments.txt"
        fi
        # A queued next episode (streamed playback only): the player offers it near the end.
        if [ "$REQUEST_SOURCE_MODE" = jellyfin ] && [ -n "$REQUEST_NEXT_ID" ]; then
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -osd_next"
        fi
        if [ "${#REQUEST_DURATION_TICKS}" -gt 7 ]; then
            PLAYER_EXTRA_ARGS="$PLAYER_EXTRA_ARGS -osd_duration $(printf '%s' "$REQUEST_DURATION_TICKS" | sed 's/.\{7\}$//')"
        fi
    fi
    playback_log "player_kind=$PLAYER_KIND"
    PLAYER_STARTED=$(date +%s)
    # shellcheck disable=SC2086
    LD_PRELOAD="$PLAYBACK_FFPLAY_PRELOAD" "$PLAYER_BIN" $PLAYER_EXTRA_ARGS \
        -stats \
        -autoexit \
        -fs \
        -vf "hflip,vflip,split=2[main][tap];[tap]select=isnan(prev_selected_t)+gte(t-prev_selected_t\,5)+lte(t-prev_selected_t\,-5),showinfo,nullsink;[main]null" \
        -i "$PLAY_URL" \
        >> "$APP_DIR/playback-ffplay.log" 2>&1 &
else
    "$PLAYBACK_FFPLAY_BIN" \
        -stats \
        -autoexit \
        -vf "hflip,vflip,split=2[main][tap];[tap]select=isnan(prev_selected_t)+gte(t-prev_selected_t\,5)+lte(t-prev_selected_t\,-5),showinfo,nullsink;[main]null" \
        -i "$PLAY_URL" \
        >> "$APP_DIR/playback-ffplay.log" 2>&1 &
fi

FFPLAY_PID=$!
playback_log "ffplay_spawned pid=$FFPLAY_PID"
if [ -r "/proc/$FFPLAY_PID/status" ]; then
    FFPLAY_STATE=$(awk '/^State:/{print $2; exit}' "/proc/$FFPLAY_PID/status" 2>/dev/null)
    playback_log "ffplay_process_state=${FFPLAY_STATE:-unavailable}"
else
    playback_log "ffplay_process_state=unavailable"
fi
wait "$FFPLAY_PID"
FFPLAY_EXIT=$?
playback_log "FFplay exited with code $FFPLAY_EXIT pid=$FFPLAY_PID (reaped)"
FFPLAY_PID=""

# A fork that dies within seconds (missing library, crash on start) must not
# cost the user their video: run the stock player once with the same arguments.
if [ "$PLAYBACK_MODE" = onion ] && [ "$PLAYER_KIND" = fork ] && [ "$FFPLAY_EXIT" -ne 0 ] \
    && [ $(( $(date +%s) - PLAYER_STARTED )) -lt 4 ]; then
    playback_log "player_fallback fork_exit=$FFPLAY_EXIT -> stock ffplay"
    LD_PRELOAD="$PLAYBACK_FFPLAY_PRELOAD" ./bin/ffplay \
        -stats \
        -autoexit \
        -fs \
        -vf "hflip,vflip,split=2[main][tap];[tap]select=isnan(prev_selected_t)+gte(t-prev_selected_t\,5)+lte(t-prev_selected_t\,-5),showinfo,nullsink;[main]null" \
        -i "$PLAY_URL" \
        >> "$APP_DIR/playback-ffplay.log" 2>&1 &
    FFPLAY_PID=$!
    wait "$FFPLAY_PID"
    FFPLAY_EXIT=$?
    playback_log "FFplay (stock fallback) exited with code $FFPLAY_EXIT pid=$FFPLAY_PID (reaped)"
    FFPLAY_PID=""
fi

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
        stop_and_reap_child reporter "$REPORTER_PID"
    else
        playback_log "Reporter exited cleanly"
        stop_and_reap_child reporter "$REPORTER_PID"
    fi
    REPORTER_PID=""
fi

# -------------------------------------------------------------------
# Next episode: the player exits with 10 when the viewer accepted (or let run) the
# up-next prompt.  Reporting for this episode is finished above; start the next one
# by rewriting the request and running this script again.
# -------------------------------------------------------------------
if [ "$FFPLAY_EXIT" = 10 ] && [ -n "$REQUEST_NEXT_ID" ] && [ "$REQUEST_SOURCE_MODE" = jellyfin ]; then
    playback_log "next_episode requested"
    cleanup_playback
    {
        printf 'item_id=%s\n' "$REQUEST_NEXT_ID"
        printf 'item_type=episode\n'
        printf 'resume_ticks=0\n'
        printf 'source_mode=jellyfin\n'
        [ "$REQUEST_NEXT_DURATION" != 0 ] && printf 'duration_ticks=%s\n' "$REQUEST_NEXT_DURATION"
    } > playback-request.txt.next && mv playback-request.txt.next playback-request.txt
    trap - EXIT
    exec sh "$APP_DIR/playback_runner.sh"
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
