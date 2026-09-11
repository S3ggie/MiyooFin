#!/bin/sh
# Development-only graceful MiyooFin exit through the normal Action::Exit path.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Override MIYOO_SSH_PORT=22 only for emergency access to Onion's fallback SSH.
. "$SCRIPT_DIR/ssh-common.sh"
TARGET=$MIYOO_SSH_TARGET

miyoo_ssh "$TARGET" sh -s <<'REMOTE_SCRIPT'
set -eu
APP_DIR=/mnt/SDCARD/App/MiyooFin
EXIT_HELPER=/tmp/miyoofin-graceful-exit
NULL_SINK=/tmp/.miyoofin-onion-remote-exit-null.$$
cleanup() { rm -f "$NULL_SINK"; }
trap cleanup EXIT HUP INT TERM
: > "$NULL_SINK"
fail() { printf '[onion-remote-exit] ERROR: %s\n' "$*" >&2; exit 1; }
count_comm() {
    name=$1
    count=0
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        [ "$(cat "$proc/comm" 2>"$NULL_SINK" || true)" = "$name" ] || continue
        count=$((count + 1))
    done
    printf '%s\n' "$count"
}
[ -x "$EXIT_HELPER" ] || fail 'classification=helper_unavailable'
[ "$(count_comm MainUI)" -eq 0 ] || fail 'MainUI is resident while MiyooFin is active'
if ! helper_output=$("$EXIT_HELPER" 2>&1); then
    case "$helper_output" in
        *classification=*) fail "graceful-exit helper rejected target (${helper_output##*classification=})" ;;
        *) fail 'graceful-exit helper rejected target (classification=unclassified_helper_failure)' ;;
    esac
fi
seconds=45
while [ "$seconds" -gt 0 ]; do
    [ "$(count_comm miyoofin)" -eq 0 ] && break
    sleep 1
    seconds=$((seconds - 1))
done
[ "$(count_comm miyoofin)" -eq 0 ] || fail 'MiyooFin did not exit after SIGUSR1'
seconds=45
while [ "$seconds" -gt 0 ]; do
    [ "$(count_comm MainUI)" -eq 1 ] && break
    sleep 1
    seconds=$((seconds - 1))
done
[ "$(count_comm MainUI)" -eq 1 ] || fail 'Onion MainUI did not return'
[ "$(count_comm miyoofin)" -eq 0 ] || fail 'MiyooFin remained resident after MainUI returned'
printf '%s\n' '[onion-remote-exit] MiyooFin exited gracefully; MainUI restored'
REMOTE_SCRIPT
