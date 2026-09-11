#!/bin/sh
# Request and verify a complete normal Onion reboot through port 2222.
set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MIYOO_SSH_CONNECT_TIMEOUT=${MIYOO_REBOOT_CONNECT_TIMEOUT:-3}
. "$SCRIPT_DIR/ssh-common.sh"
REBOOT_HELPER=/tmp/miyoofin-reboot
POLL_SECONDS=${MIYOO_REBOOT_POLL_SECONDS:-90}
MODE=normal
MIYOO_REBOOT_RECOVERY=--recovery
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != '--recovery' ]; }; then
    fail() { printf '[onion-remote-reboot] ERROR: %s\n' "$*" >&2; exit 1; }
    fail 'usage: onion-remote-reboot.sh [--recovery]'
fi
[ "$#" -eq 0 ] || MODE=recovery
fail() { printf '[onion-remote-reboot] ERROR: %s\n' "$*" >&2; exit 1; }

check_state() {
    miyoo_ssh "$MIYOO_SSH_TARGET" sh -s <<'REMOTE'
set -eu
count_comm() {
    name=$1
    count=0
    sink=/tmp/.miyoofin-reboot-null.$$
    : > "$sink"
    for proc in /proc/[0-9]*; do
        [ -r "$proc/comm" ] || continue
        [ "$(cat "$proc/comm" 2>"$sink" || true)" = "$name" ] || continue
        count=$((count + 1))
    done
    rm -f "$sink"
    printf '%s\n' "$count"
}

[ "$(id -u)" -eq 1000 ] || { echo 'wrong uid' >&2; exit 1; }
[ -x /tmp/miyoofin-reboot ] || { echo 'reboot helper unavailable' >&2; exit 1; }
[ "$(count_comm miyoofin)" -eq 0 ] || { echo 'MiyooFin is resident' >&2; exit 1; }
[ "$(count_comm MainUI)" -eq 1 ] || { echo 'MainUI is not uniquely resident' >&2; exit 1; }
[ ! -e /tmp/cmd_to_run.sh ] || { echo 'staged launch queue exists' >&2; exit 1; }
[ ! -e /mnt/SDCARD/.tmp_update/cmd_to_run.sh ] || { echo 'persistent launch queue exists' >&2; exit 1; }
printf '%s\n' READY
REMOTE
}

boot_before=$(miyoo_ssh "$MIYOO_SSH_TARGET" 'cat /proc/sys/kernel/random/boot_id') || fail 'could not read current boot identity'

if [ "$MODE" = normal ]; then
    check_state >/tmp/.miyoofin-reboot-preflight.$$ 2>&1 || {
        rm -f /tmp/.miyoofin-reboot-preflight.$$
        fail 'preflight failed'
    }
    preflight=$(sed -n '$p' /tmp/.miyoofin-reboot-preflight.$$)
    rm -f /tmp/.miyoofin-reboot-preflight.$$
    [ "$preflight" = READY ] || fail 'unexpected preflight response'
fi

printf '%s\n' "[onion-remote-reboot] requesting $MODE reboot"
if [ "$MODE" = recovery ]; then
    miyoo_ssh "$MIYOO_SSH_TARGET" "$REBOOT_HELPER" "$MIYOO_REBOOT_RECOVERY" >/tmp/.miyoofin-reboot-request.$$ 2>&1 || true
else
    miyoo_ssh "$MIYOO_SSH_TARGET" "$REBOOT_HELPER" >/tmp/.miyoofin-reboot-request.$$ 2>&1 || true
fi

down=0
seconds=$POLL_SECONDS
while [ "$seconds" -gt 0 ]; do
    if ! miyoo_ssh "$MIYOO_SSH_TARGET" true >/tmp/.miyoofin-reboot-probe.$$ 2>&1; then
        down=1
        rm -f /tmp/.miyoofin-reboot-probe.$$
        break
    fi
    rm -f /tmp/.miyoofin-reboot-probe.$$
    boot_now=$(miyoo_ssh "$MIYOO_SSH_TARGET" 'cat /proc/sys/kernel/random/boot_id' 2>/tmp/.miyoofin-reboot-probe.$$ || true)
    rm -f /tmp/.miyoofin-reboot-probe.$$
    if [ -n "$boot_now" ] && [ "$boot_now" != "$boot_before" ]; then
        down=1
        break
    fi
    sleep 1
    seconds=$((seconds - 1))
done
[ "$down" -eq 1 ] || fail 'device never left the network'

seconds=$POLL_SECONDS
returned=0
while [ "$seconds" -gt 0 ]; do
    if miyoo_ssh "$MIYOO_SSH_TARGET" true >/tmp/.miyoofin-reboot-probe.$$ 2>&1; then
        returned=1
        rm -f /tmp/.miyoofin-reboot-probe.$$
        break
    fi
    rm -f /tmp/.miyoofin-reboot-probe.$$
    sleep 1
    seconds=$((seconds - 1))
done
[ "$returned" -eq 1 ] || fail 'device did not return to the network'

postflight=$(check_state) || fail 'post-reboot state validation failed'
[ "$(printf '%s\n' "$postflight" | tail -n 1)" = READY ] || fail 'post-reboot state is not ready'
boot_after=$(miyoo_ssh "$MIYOO_SSH_TARGET" 'cat /proc/sys/kernel/random/boot_id') || fail 'could not read post-reboot boot identity'
[ "$boot_before" != "$boot_after" ] || fail 'SSH returned without a new boot'

post_helpers=$(miyoo_ssh "$MIYOO_SSH_TARGET" sh -s <<'REMOTE'
set -eu
[ "$(ps | awk '/dropbear -s -p 2222 / && !/-2 [0-9]/ && !/awk/ { count++ } END { print count + 0 }')" -eq 1 ]
[ "$(stat -c '%u:%a' /tmp/miyoofin-mainui-handoff)" = 0:4755 ]
[ "$(stat -c '%u:%a' /tmp/miyoofin-graceful-exit)" = 0:4755 ]
[ "$(stat -c '%u:%a' /tmp/miyoofin-reboot)" = 0:4755 ]
printf '%s\n' READY
REMOTE
) || fail 'post-reboot helper validation failed'
[ "$post_helpers" = READY ] || fail 'reboot helper installation is not ready'
if [ "$MODE" = recovery ]; then
    diagnostics=/tmp/miyoofin-recovery-reboot-$(date +%s).log
    cat /tmp/.miyoofin-reboot-request.$$ >"$diagnostics" 2>/tmp/.miyoofin-reboot-diag-null.$$ || true
    miyoo_ssh "$MIYOO_SSH_TARGET" 'cat /mnt/SDCARD/App/MiyooFin/telemetry-logs/recovery-reboot.log' >>"$diagnostics" 2>/tmp/.miyoofin-reboot-diag-null.$$ || true
    rm -f /tmp/.miyoofin-reboot-request.$$ /tmp/.miyoofin-reboot-diag-null.$$
    printf '%s\n' "[onion-remote-reboot] recovery diagnostics saved to $diagnostics"
else
    rm -f /tmp/.miyoofin-reboot-request.$$
fi
printf '%s\n' '[onion-remote-reboot] reboot completed and postflight checks passed'
