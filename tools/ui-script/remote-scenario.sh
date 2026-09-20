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
RUNNER_NAME=
DEFAULT_SAMPLE_INTERVAL=3
MAX_SAMPLE_INTERVAL=30

usage() {
    cat <<EOF
usage: $0 [--dry-run] [--server-unavailable] [--timeout-s N] [--log PATH] <download-home-absent|stability|home-reentry|physical-playback|suspend>

Supported:
  download-home-absent start the existing dl-start flow, then observe the
                       download store while the UI navigates away from Home
  stability           bounded repeated Home re-entry with periodic process,
                       RSS, thread, log, and duplicate-process checks
  home-reentry       leave Home and return repeatedly through scripted tabs
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
BACKUP=launch.sh.uiscript-bak
SERVER_BACKUP=server.txt.uiscript-bak
SESSION_BACKUP=session.txt.uiscript-bak
OUT="$ROOT/output/ui-script/remote-$NAME"

validate_remote_path "$APP_DIR" MIYOOFIN_DEVICE_APP_DIR
validate_remote_path "$DOWNLOAD_ROOT" MIYOOFIN_REMOTE_DOWNLOAD_ROOT
validate_remote_path "$REMOTE_LOG" MIYOOFIN_REMOTE_LOG
normalize_sample_interval

RUNDIR=$(mktemp -d /tmp/miyoofin-remote-scenario.XXXXXX)
OBSERVATIONS="$OUT/observations.log"
RUN_PID=
OBSERVER_FAILED=0
SAMPLE_COUNT=0
IN_CLEANUP=0

mkdir -p "$OUT"
: >"$OBSERVATIONS"

sample_remote() {
    _rs_sample="$RUNDIR/sample.txt"
    case "$NAME" in
        download-home-absent|stability) _rs_download_root=$DOWNLOAD_ROOT ;;
        *) _rs_download_root= ;;
    esac
    if remote_observer_snapshot "$TARGET" "$_rs_sample" "$REMOTE_LOG" 40 "$_rs_download_root"; then
        SAMPLE_COUNT=$((SAMPLE_COUNT + 1))
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
    fi
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
        if ! remote_observer_wait_postcondition "$TARGET" clean \
            "$POSTCONDITION_TIMEOUT" 3 "$APP_DIR" "$BACKUP" \
            "$SERVER_BACKUP" "$SESSION_BACKUP"; then
            echo "remote-scenario: CRITICAL: clean MainUI/launcher/config postcondition was not proven" >&2
            _rs_rc=1
        else
            echo "remote-scenario: cleanup/restoration postcondition OK"
        fi
    fi
    if [ "$OBSERVER_FAILED" = 1 ]; then
        echo "remote-scenario: CRITICAL: at least one remote observer sample failed" >&2
        _rs_rc=1
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
    if [ "$SERVER_UNAVAILABLE" = 1 ]; then
        sh "$DEVICE_RUN" --dry-run --server-unavailable --timeout-s "$TIMEOUT_S" "$RUNNER_NAME"
    else
        sh "$DEVICE_RUN" --dry-run --timeout-s "$TIMEOUT_S" "$RUNNER_NAME"
    fi
    exit $?
fi

sample_remote
if [ "$SERVER_UNAVAILABLE" = 1 ]; then
    sh "$DEVICE_RUN" --server-unavailable --timeout-s "$TIMEOUT_S" "$RUNNER_NAME" &
else
    sh "$DEVICE_RUN" --timeout-s "$TIMEOUT_S" "$RUNNER_NAME" &
fi
RUN_PID=$!

while kill -0 "$RUN_PID" 2>/dev/null; do
    sleep_until_next_sample "$SAMPLE_INTERVAL" || break
    sample_remote
done

if wait "$RUN_PID"; then
    RUN_RC=0
else
    RUN_RC=$?
fi
RUN_PID=
sample_remote
exit "$RUN_RC"
