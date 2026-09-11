#!/bin/sh
# Development-only Onion-native MiyooFin launcher.
#
# This helper queues the same /tmp/cmd_to_run.sh handoff that MainUI uses.
# The installed Onion runtime consumes it only after MainUI has exited.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Override MIYOO_SSH_PORT=22 only for emergency access to Onion's fallback SSH.
. "$SCRIPT_DIR/ssh-common.sh"
TARGET=$MIYOO_SSH_TARGET
MODE=normal

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [--telemetry]" >&2
    exit 2
fi
if [ "$#" -eq 1 ]; then
    [ "$1" = '--telemetry' ] || {
        echo "usage: $0 [--telemetry]" >&2
        exit 2
    }
    MODE=telemetry
fi

miyoo_ssh "$TARGET" sh -s -- "$MODE" <<'REMOTE_SCRIPT'
set -eu

SYS_DIR=/mnt/SDCARD/.tmp_update
APP_DIR=/mnt/SDCARD/App/MiyooFin
RUNTIME_SCRIPT=$SYS_DIR/runtime.sh
RUNTIME_QUEUE=$SYS_DIR/cmd_to_run.sh
STAGED_QUEUE=/tmp/cmd_to_run.sh
LOCK_DIR=/tmp/miyoofin-onion-remote-launch.lock
QUEUE_TMP=/tmp/.miyoofin-onion-remote-queue.$$
LEASE=/tmp/.miyoofin-onion-remote-lease.$$
NULL_SINK=/tmp/.miyoofin-onion-remote-null.$$
HANDOFF_HELPER=/tmp/miyoofin-mainui-handoff
LAUNCH_MODE=${1-normal}
[ "$LAUNCH_MODE" = normal ] || [ "$LAUNCH_MODE" = telemetry ] || {
    printf '%s\n' '[onion-remote] ERROR: invalid launch mode' >&2
    exit 2
}
TELEMETRY_LINE=
if [ "$LAUNCH_MODE" = telemetry ]; then
    LAUNCH_MODE=telemetry
    TELEMETRY_LINE='export MIYOOFIN_TELEMETRY=1'
fi
TRACE_DIR=$APP_DIR/telemetry-logs
TRACE_FILE=$TRACE_DIR/telemetry.mft
STAGED_QUEUE_OWNED=0

log() {
    printf '[onion-remote] %s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >&2
}

fail() {
    log "ERROR: $*"
    exit 1
}

cleanup() {
    rm -f "$QUEUE_TMP" "$LEASE" "$NULL_SINK"
    if [ "$STAGED_QUEUE_OWNED" -eq 1 ]; then
        rm -f "$STAGED_QUEUE"
    fi
    rmdir "$LOCK_DIR" 2>"$NULL_SINK" || true
}
trap cleanup EXIT HUP INT TERM
: > "$NULL_SINK"

count_comm() {
    name=$1
    count=0
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        comm=$(cat "$proc/comm" 2>"$NULL_SINK" || true)
        if [ "$comm" = "$name" ]; then
            count=$((count + 1))
        fi
    done
    printf '%s\n' "$count"
}

pid_for_comm() {
    name=$1
    found=
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        comm=$(cat "$proc/comm" 2>"$NULL_SINK" || true)
        if [ "$comm" = "$name" ]; then
            [ -z "$found" ] || fail "multiple $name processes are active"
            found=${proc#/proc/}
        fi
    done
    [ -n "$found" ] || fail "$name is not resident"
    printf '%s\n' "$found"
}

runtime_count() {
    count=0
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        comm=$(cat "$proc/comm" 2>"$NULL_SINK" || true)
        [ "$comm" = runtime.sh ] || continue
        cmd=$(tr '\0' ' ' < "$proc/cmdline" 2>"$NULL_SINK" || true)
        case "$cmd" in
            *"$RUNTIME_SCRIPT"*) count=$((count + 1)) ;;
        esac
    done
    printf '%s\n' "$count"
}

wait_for_no_comm() {
    name=$1
    seconds=$2
    while [ "$seconds" -gt 0 ]; do
        [ "$(count_comm "$name")" -eq 0 ] && return 0
        sleep 1
        seconds=$((seconds - 1))
    done
    [ "$(count_comm "$name")" -eq 0 ]
}

wait_for_comm() {
    name=$1
    seconds=$2
    while [ "$seconds" -gt 0 ]; do
        [ "$(count_comm "$name")" -eq 1 ] && return 0
        sleep 1
        seconds=$((seconds - 1))
    done
    [ "$(count_comm "$name")" -eq 1 ]
}

[ -x "$RUNTIME_SCRIPT" ] || fail "expected Onion runtime is missing: $RUNTIME_SCRIPT"
[ -d /proc ] || fail 'procfs is unavailable; refusing to infer process ownership'
[ "$(runtime_count)" -eq 1 ] || fail 'expected exactly one installed Onion runtime'
[ "$(count_comm MainUI)" -eq 1 ] || fail 'MainUI is not the sole foreground UI'
[ "$(count_comm miyoofin)" -eq 0 ] || fail 'MiyooFin is already active; close it physically first'

for active_name in retroarch drastic gameSwitcher advmenu; do
    [ "$(count_comm "$active_name")" -eq 0 ] || fail "$active_name is already active"
done

[ ! -e "$RUNTIME_QUEUE" ] || fail 'Onion runtime queue already exists; refusing stale state'
[ ! -e "$STAGED_QUEUE" ] || fail 'staged Onion command already exists; refusing stale state'
[ ! -e /tmp/.offOrder ] || fail 'Onion shutdown is in progress'
[ -x "$APP_DIR/launch.sh" ] || fail 'packaged MiyooFin launcher is not executable'

if [ "$LAUNCH_MODE" = telemetry ]; then
    trace_before=$(stat -c '%Y:%s:%i' "$TRACE_FILE" 2>"$NULL_SINK" || true)
else
    trace_before=
fi

mkdir "$LOCK_DIR" 2>"$NULL_SINK" || fail 'another Onion-native launch helper is active'
trap cleanup EXIT HUP INT TERM
: > "$LEASE"
deadline=$(( $(date +%s) + 120 ))

{
    printf '%s\n' '#!/bin/sh'
    printf 'LEASE=%s\n' "$LEASE"
    printf 'DEADLINE=%s\n' "$deadline"
    printf '%s\n' '[ -f "$LEASE" ] || exit 75'
    printf '%s\n' 'now=$(date +%s)'
    printf '%s\n' '[ "$now" -lt "$DEADLINE" ] || { rm -f "$LEASE"; exit 75; }'
    printf '%s\n' 'rm -f "$LEASE"'
    printf '%s\n' "cd '$APP_DIR' || exit 1"
    if [ "$LAUNCH_MODE" = telemetry ]; then
        printf '%s\n' "$TELEMETRY_LINE"
    fi
    printf '%s\n' "exec '$APP_DIR/launch.sh'"
} > "$QUEUE_TMP" || fail 'could not stage Onion command'
chmod 700 "$QUEUE_TMP"
mv -f "$QUEUE_TMP" "$STAGED_QUEUE" || fail 'could not atomically queue Onion command'
STAGED_QUEUE_OWNED=1
log "queued Onion-native MiyooFin handoff mode=$LAUNCH_MODE"

[ -x "$HANDOFF_HELPER" ] || fail 'privileged MainUI handoff helper is unavailable'
"$HANDOFF_HELPER" || fail 'privileged MainUI handoff helper rejected the launch'
STAGED_QUEUE_OWNED=0

log 'requested MainUI exit through validated privileged handoff'

wait_for_no_comm MainUI 30 || fail 'MainUI did not exit after the queued handoff'
log 'MainUI exited; waiting for Onion runtime to launch MiyooFin'

wait_for_comm miyoofin 45 || fail 'MiyooFin did not become resident through Onion runtime'
[ "$(count_comm MainUI)" -eq 0 ] || fail 'MainUI is concurrently resident with MiyooFin'
log 'MiyooFin is the only foreground UI; use physical controls, then exit normally'

app_seconds=1800
while [ "$(count_comm miyoofin)" -gt 0 ]; do
    [ "$(count_comm MainUI)" -eq 0 ] || fail 'MainUI returned while MiyooFin was active'
    sleep 1
    app_seconds=$((app_seconds - 1))
    [ "$app_seconds" -gt 0 ] || fail 'timed out waiting for physical MiyooFin exit'
done
log 'MiyooFin exited; waiting for Onion MainUI restoration'

wait_for_comm MainUI 45 || fail 'MainUI did not return after MiyooFin exit'
[ "$(count_comm miyoofin)" -eq 0 ] || fail 'MiyooFin remained resident after MainUI returned'
[ "$(runtime_count)" -eq 1 ] || fail 'installed Onion runtime did not remain singular'
[ ! -e "$RUNTIME_QUEUE" ] || fail 'Onion runtime queue remained stale after app exit'

if [ -n "$TELEMETRY_LINE" ]; then
    trace_after=$(stat -c '%Y:%s:%i' "$TRACE_FILE" 2>"$NULL_SINK" || true)
    [ -n "$trace_after" ] || fail 'telemetry mode produced no telemetry trace'
    [ "$trace_after" != "$trace_before" ] || fail 'telemetry trace was not refreshed'
    log 'telemetry trace refreshed without exposing its contents'
fi

log 'Onion-native MiyooFin launch completed and MainUI is restored'
REMOTE_SCRIPT
