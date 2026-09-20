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
wait_for_mainui() {
    seconds=45
    while [ "$seconds" -gt 0 ]; do
        [ "$(count_comm MainUI)" -eq 1 ] && break
        sleep 1
        seconds=$((seconds - 1))
    done
    [ "$(count_comm MainUI)" -eq 1 ] || fail 'Onion MainUI did not return'
    [ "$(count_comm miyoofin)" -eq 0 ] || fail 'MiyooFin remained resident after MainUI returned'
}
[ -x "$EXIT_HELPER" ] || fail 'classification=helper_unavailable'
[ "$(count_comm MainUI)" -eq 0 ] || fail 'MainUI is resident while MiyooFin is active'

# The app normally exits itself after a scripted QUIT.  The caller can race
# that exit between its process check and this helper, in which case the
# setuid bridge correctly reports that there is no validated target.  Treat
# that already-safe state as success, but only after proving MainUI returned
# and MiyooFin is gone.  A live-but-unvalidated process still fails closed.
if [ "$(count_comm miyoofin)" -eq 0 ]; then
    wait_for_mainui
    printf '%s\n' '[onion-remote-exit] MiyooFin was already gone; MainUI restored'
    exit 0
fi
if ! helper_output=$("$EXIT_HELPER" 2>&1); then
    case "$helper_output" in
        *classification=target_validation_failed*)
            # Re-check after the helper's validation.  This is the same safe
            # race as above; never accept the error while miyoofin remains.
            if [ "$(count_comm miyoofin)" -eq 0 ]; then
                wait_for_mainui
                printf '%s\n' '[onion-remote-exit] MiyooFin exited during validation; MainUI restored'
                exit 0
            fi
            fail "graceful-exit helper rejected target (target_validation_failed; MiyooFin still resident)"
            ;;
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
wait_for_mainui
printf '%s\n' '[onion-remote-exit] MiyooFin exited gracefully; MainUI restored'
REMOTE_SCRIPT
