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
fail() { printf '[onion-remote-exit] ERROR: %s\n' "$*" >&2; exit 1; }
count_comm() {
    name=$1
    count=0
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        [ "$(cat "$proc/comm" 2>/dev/null || true)" = "$name" ] || continue
        count=$((count + 1))
    done
    printf '%s\n' "$count"
}
miyoofin_pid=
for proc in /proc/[0-9]*; do
    [ -r "$proc/comm" ] || continue
    [ "$(cat "$proc/comm" 2>/dev/null || true)" = miyoofin ] || continue
    [ -z "$miyoofin_pid" ] || fail 'multiple MiyooFin processes are active'
    miyoofin_pid=$(basename "$proc")
done
[ -n "$miyoofin_pid" ] || fail 'MiyooFin is not resident'
[ "$(count_comm MainUI)" -eq 0 ] || fail 'MainUI is resident while MiyooFin is active'
exe=$(readlink "/proc/$miyoofin_pid/exe" 2>/dev/null || true)
[ "$(basename "$exe")" = miyoofin ] || fail "target PID is not MiyooFin: $exe"
[ -x "$APP_DIR/miyoofin" ] || fail 'installed MiyooFin executable is missing'
kill -USR1 "$miyoofin_pid" || fail 'SIGUSR1 graceful-exit request failed'
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
