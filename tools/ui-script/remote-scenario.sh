#!/bin/sh
# Remote scenario wrapper for observer-backed device flows.
#
# This wrapper does not replace device-run.sh.  It delegates launch, input,
# screenshot handling, and launcher/config restoration to that established
# runner, while sampling remote state and requiring the restored postcondition
# before reporting success.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
DEVICE_RUN="$SCRIPT_DIR/device-run.sh"
NAME=
SERVER_UNAVAILABLE=0
DRY_RUN=0
TIMEOUT_S=${MIYOOFIN_UI_TIMEOUT_S:-300}
SAMPLE_INTERVAL=${MIYOOFIN_REMOTE_SAMPLE_INTERVAL_S:-3}
POSTCONDITION_TIMEOUT=${MIYOOFIN_REMOTE_POSTCONDITION_TIMEOUT_S:-45}
REMOTE_LOG=${MIYOOFIN_REMOTE_LOG:-/tmp/miyoofin-ui-script/app.log}
PLAYBACK_INTERVAL=${MIYOOFIN_OFFLINE_PLAYBACK_INTERVAL_S:-30}
RUNNER_NAME=
DEFAULT_SAMPLE_INTERVAL=3
MAX_SAMPLE_INTERVAL=30
DEFAULT_PLAYBACK_INTERVAL=30
MAX_PLAYBACK_INTERVAL=300

usage() {
    cat <<EOF
usage: $0 [--dry-run] [--server-unavailable] [--timeout-s N] [--log PATH] <download-home-absent|stability|home-reentry|offline-playback|physical-playback|suspend>

Supported:
  download-home-absent start the existing dl-start flow, then observe the
                       download store while the UI navigates away from Home
  stability           bounded repeated Home re-entry with periodic process,
                       RSS, thread, log, and duplicate-process checks
  home-reentry       leave Home and return repeatedly through scripted tabs
  offline-playback   select the first confirmed completed Downloads row,
                     launch local playback, sample bounded player evidence,
                     then request the supported graceful exit
Unsupported (reported as UNRUN, never as PASS):
  physical-playback  requires physical video/audio output
  suspend            requires physical suspend/resume and wake behavior
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --server-unavailable) SERVER_UNAVAILABLE=1; shift ;;
        --timeout-s) TIMEOUT_S=${2:?--timeout-s needs a value}; shift 2 ;;
        --timeout-s=*) TIMEOUT_S=${1#--timeout-s=}; shift ;;
        --log) REMOTE_LOG=${2:?--log needs a path}; shift 2 ;;
        --log=*) REMOTE_LOG=${1#--log=}; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) echo "remote-scenario: unknown flag: $1" >&2; exit 2 ;;
        *) NAME=$1; shift ;;
    esac
done
[ -n "$NAME" ] || { usage >&2; exit 2; }

validate_remote_path() {
    _rsp_path=$1
    _rsp_name=$2
    case "$_rsp_path" in
        /*) ;;
        *) echo "remote-scenario: $_rsp_name must be an absolute path" >&2; exit 2 ;;
    esac
    case "$_rsp_path" in
        *[!A-Za-z0-9_./-]*)
            echo "remote-scenario: $_rsp_name contains unsafe characters" >&2
            exit 2
            ;;
    esac
}

normalize_sample_interval() {
    case "$SAMPLE_INTERVAL" in
        ''|*[!0-9]*) SAMPLE_INTERVAL=$DEFAULT_SAMPLE_INTERVAL ;;
    esac
    while [ "$SAMPLE_INTERVAL" != 0 ] &&
          [ "${SAMPLE_INTERVAL#0}" != "$SAMPLE_INTERVAL" ]; do
        SAMPLE_INTERVAL=${SAMPLE_INTERVAL#0}
    done
    case "$SAMPLE_INTERVAL" in
        0) SAMPLE_INTERVAL=1 ;;
        [1-9]|[12][0-9]|30) ;;
        *) SAMPLE_INTERVAL=$MAX_SAMPLE_INTERVAL ;;
    esac
}

normalize_playback_interval() {
    case "$PLAYBACK_INTERVAL" in
        ''|*[!0-9]*) PLAYBACK_INTERVAL=$DEFAULT_PLAYBACK_INTERVAL ;;
    esac
    while [ "$PLAYBACK_INTERVAL" != 0 ] &&
          [ "${PLAYBACK_INTERVAL#0}" != "$PLAYBACK_INTERVAL" ]; do
        PLAYBACK_INTERVAL=${PLAYBACK_INTERVAL#0}
    done
    case "$PLAYBACK_INTERVAL" in
        0) PLAYBACK_INTERVAL=1 ;;
        [1-9]|[1-9][0-9]|[12][0-9][0-9]) ;;
        *) PLAYBACK_INTERVAL=$MAX_PLAYBACK_INTERVAL ;;
    esac
}

case "$NAME" in
    physical-playback|playback)
        echo "UNRUN scenario=$NAME reason=requires physical video/audio playback";
        exit 0
        ;;
    suspend|physical-suspend)
        echo "UNRUN scenario=$NAME reason=requires physical suspend/resume hardware";
        exit 0
        ;;
    download-home-absent) RUNNER_NAME=dl-start ;;
    stability|home-reentry) RUNNER_NAME=home-reentry ;;
    offline-playback) RUNNER_NAME=offline-playback ;;
    *) echo "remote-scenario: unknown scenario: $NAME" >&2; exit 2 ;;
esac

[ -x "$DEVICE_RUN" ] || { echo "remote-scenario: missing executable $DEVICE_RUN" >&2; exit 2; }
# shellcheck disable=SC1091
. "$ROOT/tools/miyoo/ssh-common.sh"
# shellcheck disable=SC1091
. "$SCRIPT_DIR/remote-observer.sh"

TARGET=$MIYOO_SSH_TARGET
APP_DIR=${MIYOOFIN_DEVICE_APP_DIR:-/mnt/SDCARD/App/MiyooFin}
DOWNLOAD_ROOT=${MIYOOFIN_REMOTE_DOWNLOAD_ROOT:-$APP_DIR/downloads}
PLAYBACK_LOG=${MIYOOFIN_REMOTE_PLAYBACK_LOG:-$APP_DIR/playback-launch.log}
PLAYBACK_LOG_OFFSET=
BACKUP=launch.sh.uiscript-bak
SERVER_BACKUP=server.txt.uiscript-bak
SESSION_BACKUP=session.txt.uiscript-bak
OUT="$ROOT/output/ui-script/remote-$NAME"
PLAYBACK_SHOT="$ROOT/output/ui-script/device-offline-playback/shots/offline-playback-downloads.bmp"

validate_remote_path "$APP_DIR" MIYOOFIN_DEVICE_APP_DIR
validate_remote_path "$DOWNLOAD_ROOT" MIYOOFIN_REMOTE_DOWNLOAD_ROOT
validate_remote_path "$REMOTE_LOG" MIYOOFIN_REMOTE_LOG
validate_remote_path "$PLAYBACK_LOG" MIYOOFIN_REMOTE_PLAYBACK_LOG
normalize_sample_interval
normalize_playback_interval

RUNDIR=$(mktemp -d /tmp/miyoofin-remote-scenario.XXXXXX)
OBSERVATIONS="$OUT/observations.log"
RUN_PID=
OBSERVER_FAILED=0
SAMPLE_COUNT=0
IN_CLEANUP=0
SUPPORTED_EXIT_REQUESTED=0
SUPPORTED_EXIT_SUCCEEDED=0
RUN_ELAPSED=0
POSTCONDITION_OK=0
BASELINE_CAPTURED=0
BASELINE_FFPLAY_PIDS=
BASELINE_BRIDGE_PIDS=
BASELINE_REPORTER_PIDS=
PLAYBACK_SHOT_EXISTED=0
PLAYBACK_SHOT_BASELINE_MTIME=
PLAYBACK_SHOT_BASELINE_HASH=
RUNNER_LOG="$RUNDIR/device-run.log"

mkdir -p "$OUT"
: >"$OBSERVATIONS"

snapshot_process_pids() {
    _rs_snapshot=$1
    _rs_process_name=$2
    awk -v wanted="name=$_rs_process_name" '
        $1 == "process" && $2 == wanted {
            in_pids = 0
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^pids=/) {
                    in_pids = 1
                    value = substr($i, 6)
                } else if (in_pids && $i ~ /^rss_kb=/) {
                    break
                } else if (in_pids) {
                    value = $i
                } else {
                    continue
                }
                if (in_pids && value ~ /^[0-9]+$/) {
                    printf "%s%s", separator, value
                    separator = " "
                }
            }
        }
    ' "$_rs_snapshot"
}

capture_playback_baseline() {
    _rs_snapshot=$1
    BASELINE_FFPLAY_PIDS=$(snapshot_process_pids "$_rs_snapshot" ffplay)
    BASELINE_BRIDGE_PIDS=$(snapshot_process_pids "$_rs_snapshot" miyoofin-https-bridge)
    BASELINE_REPORTER_PIDS=$(snapshot_process_pids "$_rs_snapshot" miyoofin-playback-reporter)
    BASELINE_CAPTURED=1
}

capture_playback_artifact_baseline() {
    if [ -f "$PLAYBACK_SHOT" ]; then
        PLAYBACK_SHOT_EXISTED=1
        PLAYBACK_SHOT_BASELINE_MTIME=$(stat -c '%Y:%y' "$PLAYBACK_SHOT" 2>/dev/null || true)
        PLAYBACK_SHOT_BASELINE_HASH=$(sha256sum "$PLAYBACK_SHOT" 2>/dev/null \
            | awk '{ print $1 }' || true)
    fi
}

sample_remote() {
    _rs_sample="$RUNDIR/sample.txt"
    case "$NAME" in
        download-home-absent|stability|offline-playback) _rs_download_root=$DOWNLOAD_ROOT ;;
        *) _rs_download_root= ;;
    esac
    _rs_playback_log=$PLAYBACK_LOG
    _rs_playback_offset=
    if [ "$NAME" = offline-playback ] && [ "$PLAYBACK_LOG_OFFSET" != -1 ]; then
        _rs_playback_log=$PLAYBACK_LOG
        _rs_playback_offset=$PLAYBACK_LOG_OFFSET
    elif [ "$NAME" = offline-playback ]; then
        _rs_playback_log=
    fi
    if remote_observer_snapshot "$TARGET" "$_rs_sample" "$REMOTE_LOG" 40 \
        "$_rs_download_root" "$_rs_playback_log" "$_rs_playback_offset"; then
        SAMPLE_COUNT=$((SAMPLE_COUNT + 1))
        if [ "$NAME" = offline-playback ] && [ "$BASELINE_CAPTURED" = 0 ]; then
            capture_playback_baseline "$_rs_sample"
        fi
        {
            printf '%s\n' "--- observer sample epoch=$(date +%s) ---"
            cat "$_rs_sample"
        } >>"$OBSERVATIONS"
        if [ "$NAME" = stability ] || [ "$NAME" = home-reentry ]; then
            if ! remote_observer_no_duplicate_processes "$_rs_sample"; then
                OBSERVER_FAILED=1
                printf '%s\n' 'observer_check duplicate MainUI/miyoofin process detected' >>"$OBSERVATIONS"
            fi
        fi
    else
        OBSERVER_FAILED=1
        printf '%s\n' "--- observer sample unavailable epoch=$(date +%s) ---" >>"$OBSERVATIONS"
    fi
}

playback_log_boundary() {
    miyoo_ssh "$TARGET" sh -s -- "$PLAYBACK_LOG" <<'EOF'
set -eu
path=$1
if [ -f "$path" ]; then
    wc -c <"$path" | tr -d '[:space:]'
else
    printf '0\n'
fi
EOF
}

completed_download_count() {
    awk '
        $1 == "download_state" {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "complete_items") value = kv[2]
            }
        }
        END { print value + 0 }
    ' "$OBSERVATIONS"
}

offline_playback_evidence() {
    completed=$(completed_download_count)
    handoff=0
    local_source=0
    player=0
    framebuffer=0
    current_run_launch=0
    playback_evidence=$(awk '
        /playback_log_tail_begin/ { in_playback_log = 1; next }
        /playback_log_tail_end/ { in_playback_log = 0; next }
        in_playback_log { print }
    ' "$OBSERVATIONS")
    grep -q 'Starting external playback handoff' "$OBSERVATIONS" && handoff=1 || true
    printf '%s\n' "$playback_evidence" | grep -q 'source_mode=local' && local_source=1 || true
    if printf '%s\n' "$playback_evidence" | grep -Eq \
        'ffplay_spawned pid=[0-9]+|Bridge running \(PID=[0-9]+\)|Reporter running \(PID=[0-9]+\)'; then
        current_run_launch=1
    fi
    if [ "$current_run_launch" -eq 1 ] || playback_pid_is_new; then
        player=1
    fi
    # The device framebuffer node is not readable on every Onion build.  The
    # UI shim's BMP capture is the portable framebuffer evidence for this
    # scenario.  Do not accept an artifact left by an earlier run.
    if [ -f "$PLAYBACK_SHOT" ]; then
        _rs_current_mtime=$(stat -c '%Y:%y' "$PLAYBACK_SHOT" 2>/dev/null || true)
        _rs_current_hash=$(sha256sum "$PLAYBACK_SHOT" 2>/dev/null \
            | awk '{ print $1 }' || true)
        if [ "$PLAYBACK_SHOT_EXISTED" = 0 ] &&
           { [ -n "$_rs_current_mtime" ] || [ -n "$_rs_current_hash" ]; }; then
            framebuffer=1
        elif [ -n "$PLAYBACK_SHOT_BASELINE_MTIME" ] &&
             [ "$_rs_current_mtime" != "$PLAYBACK_SHOT_BASELINE_MTIME" ]; then
            framebuffer=1
        elif [ -n "$PLAYBACK_SHOT_BASELINE_HASH" ] &&
             [ "$_rs_current_hash" != "$PLAYBACK_SHOT_BASELINE_HASH" ]; then
            framebuffer=1
        fi
    fi
    printf '%s %s %s %s %s\n' "$completed" "$handoff" "$local_source" "$player" "$framebuffer"
}

playback_pid_is_new() {
    [ "$BASELINE_CAPTURED" = 1 ] || return 1
    awk -v baseline_ffplay="$BASELINE_FFPLAY_PIDS" \
        -v baseline_bridge="$BASELINE_BRIDGE_PIDS" \
        -v baseline_reporter="$BASELINE_REPORTER_PIDS" '
        function baseline_has(pid, text, values, count, i) {
            count = split(text, values, / +/)
            for (i = 1; i <= count; i++) {
                if (values[i] == pid) return 1
            }
            return 0
        }
        $1 == "process" {
            if ($2 == "name=ffplay") baseline = baseline_ffplay
            else if ($2 == "name=miyoofin-https-bridge") baseline = baseline_bridge
            else if ($2 == "name=miyoofin-playback-reporter") baseline = baseline_reporter
            else next
            in_pids = 0
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^pids=/) {
                    in_pids = 1
                    pid = substr($i, 6)
                } else if (in_pids && $i ~ /^rss_kb=/) {
                    break
                } else if (in_pids) {
                    pid = $i
                } else {
                    continue
                }
                if (in_pids && pid ~ /^[0-9]+$/ && !baseline_has(pid, baseline)) {
                    found = 1
                }
            }
        }
        END { exit found ? 0 : 1 }
    ' "$OBSERVATIONS"
}

report_scenario() {
    [ "$DRY_RUN" = 0 ] || return 0
    if [ "$NAME" = download-home-absent ]; then
        if [ "$1" -ne 0 ]; then
            echo "PARTIAL scenario=download-home-absent reason=runner or cleanup did not complete cleanly"
            return 0
        fi
        awk '
            $1 == "download_state" {
                seen++
                for (i = 1; i <= NF; i++) {
                    split($i, kv, "=")
                    if (kv[1] == "complete_items") complete = kv[2]
                    if (kv[1] == "downloaded_bytes") bytes = kv[2]
                    if (kv[1] == "media_files") media = kv[2]
                    if (kv[1] == "partial_files") partial = kv[2]
                }
                if (seen == 1) {
                    first_complete = complete
                    first_bytes = bytes
                    first_media = media
                    first_partial = partial
                }
                last_complete = complete
                last_bytes = bytes
                last_media = media
                last_partial = partial
            }
            END {
                if (seen < 2) {
                    print "PARTIAL scenario=download-home-absent reason=download state had fewer than two samples"
                } else if (last_complete > first_complete) {
                    print "PASS scenario=download-home-absent evidence=download manifest completion state advanced"
                } else if (last_bytes > first_bytes || last_media > first_media || last_partial != first_partial) {
                    print "PARTIAL scenario=download-home-absent evidence=download progress changed; completion not proven"
                } else {
                    print "PARTIAL scenario=download-home-absent reason=no download progress or completion delta observed"
                }
            }
        ' "$OBSERVATIONS"
    elif [ "$NAME" = stability ] || [ "$NAME" = home-reentry ]; then
        if [ "$1" -eq 0 ] && [ "$SAMPLE_COUNT" -ge 3 ] && [ "$OBSERVER_FAILED" = 0 ]; then
            echo "PASS scenario=$NAME semantics=stability-alias samples=$SAMPLE_COUNT duplicate-process-check=passed"
        else
            echo "PARTIAL scenario=$NAME semantics=stability-alias samples=$SAMPLE_COUNT reason=periodic samples or duplicate-process checks were incomplete"
        fi
    elif [ "$NAME" = offline-playback ]; then
        _rs_rc=$1
        set -- $(offline_playback_evidence)
        _rs_completed=$1
        _rs_handoff=$2
        _rs_local=$3
        _rs_player=$4
        _rs_framebuffer=$5
        if [ "$_rs_rc" -eq 0 ] && [ "$_rs_completed" -gt 0 ] &&
           [ "$SUPPORTED_EXIT_SUCCEEDED" = 1 ] &&
           [ "$_rs_handoff" -eq 1 ] && [ "$_rs_local" -eq 1 ] &&
           [ "$_rs_player" -eq 1 ] && [ "$_rs_framebuffer" -eq 1 ]; then
            echo "PASS scenario=offline-playback scope=ui-player-process completed_items=$_rs_completed samples=$SAMPLE_COUNT player-process=observed framebuffer=observed supported-exit=verified"
            echo "PARTIAL scenario=offline-playback scope=audio-video reason=audio/video output was not instrumented; no claim of audible or visible media playback"
        else
            echo "PARTIAL scenario=offline-playback scope=ui-player-process completed_items=$_rs_completed samples=$SAMPLE_COUNT handoff=$_rs_handoff local-source=$_rs_local player-process=$_rs_player framebuffer=$_rs_framebuffer supported-exit=$SUPPORTED_EXIT_SUCCEEDED reason=UI/player-process evidence, supported exit, or clean restoration was incomplete"
        fi
    fi
}

runner_failure_is_expected_bounded_exit() {
    _rs_runner_rc=${1:-${RUN_RC:-0}}
    [ "$NAME" = offline-playback ] || return 1
    [ "$_rs_runner_rc" -ne 0 ] || return 1
    [ "$SUPPORTED_EXIT_SUCCEEDED" = 1 ] || return 1
    [ -f "$RUNNER_LOG" ] || return 1
    grep -Fq \
        'device-run(offline-playback): no clean script verdict (app exited;' \
        "$RUNNER_LOG" || return 1
    ! grep -Fq 'device-run: CRITICAL:' "$RUNNER_LOG"
}

offline_playback_cleanup_override_allowed() {
    _rs_runner_rc=${1:-${RUN_RC:-0}}
    runner_failure_is_expected_bounded_exit "$_rs_runner_rc" || return 1
    [ "$SUPPORTED_EXIT_SUCCEEDED" = 1 ] &&
        [ "$POSTCONDITION_OK" = 1 ] &&
    [ "$OBSERVER_FAILED" = 0 ] || return 1
    set -- $(offline_playback_evidence)
    [ "$1" -gt 0 ] && [ "$2" -eq 1 ] && [ "$3" -eq 1 ] &&
        [ "$4" -eq 1 ] && [ "$5" -eq 1 ]
}

request_supported_exit() {
    [ "$SUPPORTED_EXIT_SUCCEEDED" = 1 ] && return 0
    [ "$SUPPORTED_EXIT_REQUESTED" = 0 ] || return 1
    SUPPORTED_EXIT_REQUESTED=1
    echo "remote-scenario: requesting supported graceful exit for bounded offline playback" >&2
    if sh "$ROOT/tools/miyoo/onion-remote-exit.sh" >"$RUNDIR/offline-playback-exit.log" 2>&1; then
        cat "$RUNDIR/offline-playback-exit.log" >>"$OBSERVATIONS"
        SUPPORTED_EXIT_SUCCEEDED=1
        return 0
    fi
    sed 's/^/supported-exit: /' "$RUNDIR/offline-playback-exit.log" >&2 || true
    cat "$RUNDIR/offline-playback-exit.log" >>"$OBSERVATIONS"
    return 1
}

sleep_until_next_sample() {
    _rs_remaining=$1
    # Check once per second so a long configured interval cannot leave the
    # wrapper asleep after the device runner has already exited.
    while [ "$_rs_remaining" -gt 0 ]; do
        kill -0 "$RUN_PID" 2>/dev/null || return 1
        sleep 1
        _rs_remaining=$((_rs_remaining - 1))
    done
    return 0
}

cleanup() {
    _rs_rc=$?
    if [ "$IN_CLEANUP" = 1 ]; then
        exit "$_rs_rc"
    fi
    IN_CLEANUP=1
    if [ "$NAME" = offline-playback ] && [ -n "$RUN_PID" ] &&
       kill -0 "$RUN_PID" 2>/dev/null && [ "$SUPPORTED_EXIT_REQUESTED" = 0 ]; then
        request_supported_exit || _rs_rc=1
    fi
    if [ -n "$RUN_PID" ] && kill -0 "$RUN_PID" 2>/dev/null; then
        echo "remote-scenario: stopping device runner; its EXIT trap owns restoration" >&2
        kill -TERM "$RUN_PID" 2>/dev/null || true
        _rs_wait=0
        while kill -0 "$RUN_PID" 2>/dev/null && [ "$_rs_wait" -lt 60 ]; do
            sleep 1
            _rs_wait=$((_rs_wait + 1))
        done
        if kill -0 "$RUN_PID" 2>/dev/null; then
            echo "remote-scenario: CRITICAL: device runner did not finish cleanup" >&2
            _rs_rc=1
        fi
    fi
    if [ "$DRY_RUN" = 0 ]; then
        _rs_postcondition=clean
        [ "$NAME" = offline-playback ] && _rs_postcondition=offline-clean
        if ! remote_observer_wait_postcondition "$TARGET" "$_rs_postcondition" \
            "$POSTCONDITION_TIMEOUT" 3 "$APP_DIR" "$BACKUP" \
            "$SERVER_BACKUP" "$SESSION_BACKUP"; then
            echo "remote-scenario: CRITICAL: clean MainUI/launcher/config postcondition was not proven" >&2
            _rs_rc=1
        else
            POSTCONDITION_OK=1
            echo "remote-scenario: cleanup/restoration postcondition OK"
        fi
    fi
    if [ "$OBSERVER_FAILED" = 1 ]; then
        echo "remote-scenario: CRITICAL: at least one remote observer sample failed" >&2
        _rs_rc=1
    fi
    if [ -f "$RUNNER_LOG" ]; then
        cat "$RUNNER_LOG"
    fi
    if [ "$NAME" = offline-playback ] &&
       offline_playback_cleanup_override_allowed "$_rs_rc"; then
        # device-run quite correctly returns non-zero when the bounded
        # scenario is ended before its script reaches QUIT.  The supported
        # exit plus the strict postcondition is the intended verdict here.
        _rs_rc=0
    fi
    report_scenario "$_rs_rc"
    rm -rf "$RUNDIR"
    RUN_PID=
    exit "$_rs_rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [ "$DRY_RUN" = 1 ]; then
    echo "remote-scenario: sampling interval=${SAMPLE_INTERVAL}s (bounded to 1s runner checks)"
    [ "$NAME" = offline-playback ] && echo "remote-scenario: offline playback interval=${PLAYBACK_INTERVAL}s; exit=onion-remote-exit.sh"
    if [ "$SERVER_UNAVAILABLE" = 1 ]; then
        sh "$DEVICE_RUN" --dry-run --server-unavailable --timeout-s "$TIMEOUT_S" "$RUNNER_NAME"
    else
        sh "$DEVICE_RUN" --dry-run --timeout-s "$TIMEOUT_S" "$RUNNER_NAME"
    fi
    exit $?
fi

if [ "$NAME" = offline-playback ]; then
    capture_playback_artifact_baseline
    if PLAYBACK_LOG_OFFSET=$(playback_log_boundary) &&
       case "$PLAYBACK_LOG_OFFSET" in ''|*[!0-9]*) false ;; *) true ;; esac; then
        :
    else
        # Never fall back to the append-only log without a run boundary: old
        # local-source/spawn entries must only make the result PARTIAL.
        echo "remote-scenario: playback log boundary unavailable; current-run playback evidence will be PARTIAL" >&2
        PLAYBACK_LOG_OFFSET=-1
    fi
fi

sample_remote
if [ "$NAME" = offline-playback ] && [ "$(completed_download_count)" -lt 1 ]; then
    echo "PARTIAL scenario=offline-playback reason=no confirmed completed download was present; refusing to launch playback"
    exit 1
fi
if [ "$SERVER_UNAVAILABLE" = 1 ]; then
    sh "$DEVICE_RUN" --server-unavailable --timeout-s "$TIMEOUT_S" "$RUNNER_NAME" \
        >"$RUNNER_LOG" 2>&1 &
else
    sh "$DEVICE_RUN" --timeout-s "$TIMEOUT_S" "$RUNNER_NAME" \
        >"$RUNNER_LOG" 2>&1 &
fi
RUN_PID=$!

while kill -0 "$RUN_PID" 2>/dev/null; do
    sleep_until_next_sample "$SAMPLE_INTERVAL" || break
    RUN_ELAPSED=$((RUN_ELAPSED + SAMPLE_INTERVAL))
    sample_remote
    if [ "$NAME" = offline-playback ] &&
       [ "$SUPPORTED_EXIT_REQUESTED" = 0 ] &&
       [ "$RUN_ELAPSED" -ge "$PLAYBACK_INTERVAL" ]; then
        request_supported_exit || OBSERVER_FAILED=1
    fi
done

if wait "$RUN_PID"; then
    RUN_RC=0
else
    RUN_RC=$?
fi
RUN_PID=
sample_remote
exit "$RUN_RC"
