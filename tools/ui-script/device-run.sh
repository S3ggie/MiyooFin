#!/bin/sh
# MiyooFin device UI-harness runner (test-only, ARM/Miyoo Mini Plus).
#
# Drives the REAL installed ARM binary on the Miyoo through the REAL
# privileged Onion launch path with a scripted-input timeline:
#   1. builds the ARM LD_PRELOAD input shim (tools/ui-script/Makefile.arm),
#   2. pushes the shim + device script (tools/ui-script/scripts/<name>-device.txt)
#      to a /tmp scratch dir on the device,
#   3. backs up the installed launcher to launch.sh.uiscript-bak ON THE
#      DEVICE with a plain `cp` (no -p), then rewrites launch.sh strictly
#      IN PLACE (no chmod, no mv, no rename) to export the shim env
#      (LD_PRELOAD + MIYOOFIN_UI_*) and redirect app stdout into the
#      scratch app log. In-place redirects preserve the root-owned 0777
#      inode, so no ownership/permission syscall can fail midway;
#   4. triggers Onion's privileged handoff
#      (tools/miyoo/onion-remote-launch.sh) so the app runs as root exactly
#      like a production launch — MainUI must be resident,
#   5. the shim replays KEYs as DEVICE scancodes and captures SCREENSHOTs
#      through the same /tmp flag + app framebuffer hook the desktop
#      harness and tools/miyoo/miyoofin-screenshot.sh use,
#   6. exits the app via tools/miyoo/onion-remote-exit.sh (or notes its
#      clean self-exit through the script's QUIT),
#   7. pulls shots + app log + verdict back, restores launch.sh from backup
#      with verification, removes scratch, and runs the SAME assertions
#      (tools/ui-script/assert_shots.py) as the desktop variant.
#
# There is deliberately NO stub server and NO reverse SSH tunnel here: the
# privileged launch uses the app's REAL session and real server, exactly as
# production does. (The stub/tunnel still exists for the desktop harness,
# tools/ui-script/run.sh, which has no server of its own.)
#
# Why privileged: launching the binary directly over SSH runs as the
# default non-root user, which CANNOT work — /dev/mi/gfx and /dev/mi/sys
# are root-only and /dev/urandom is 0660 root:root, so the app logs
# "failed to open /dev/mi/... (Permission denied)", never renders, and
# exits about a second after "[App] Initialisation complete". OnionOS
# launches the app as root via <app>/launch.sh; this harness borrows that
# path temporarily and gives it back.
#
# Usage: sh tools/ui-script/device-run.sh [--dry-run] [--desktop-keys]
#            [--timeout-s N] <smoke|series>
#   --dry-run       print every host/remote action, do nothing (no SSH).
#   --desktop-keys  do NOT set MIYOOFIN_UI_DEVICE_KEYS (shim sends WASD
#                   codes; only useful with Raw: scancodes in the script).
# Env:
#   MIYOO_SSH_TARGET / MIYOO_HOST / MIYOO_SSH_PORT (tools/miyoo/ssh-common.sh)
#   MIYOOFIN_UI_TIMEOUT_S verdict watchdog seconds (default 300).
#   MIYOOFIN_DEVICE_APP_DIR device app dir (default /mnt/SDCARD/App/MiyooFin).
#     Point it at a local fixture directory to exercise the launcher
#     surgery offline via tools/ui-script/test-launcher-roundtrip.sh
#     (the same tools/ui-script/launcher-surgery.sh functions the device
#     runs over SSH).
#
# SAFETY (unattended hardware — read before extending):
#   - NEVER reboots or power-cycles the device (no such command exists here).
#   - The ONLY writes outside /tmp/miyoofin-ui-script are the launcher
#     backup + in-place rewrite inside the app dir; both are undone by the
#     restore step, which is verified (checksum vs the pre-injection value
#     + byte-compare + marker-absent + executable-bit check).
#   - The device filesystem rejects `cp -p` and `chmod` on the root-owned
#     launcher for the unprivileged SSH user, so the surgery uses NEITHER:
#     plain `cp` for the backup, in-place `cat ... > launch.sh` redirects
#     for inject and restore. See tools/ui-script/launcher-surgery.sh.
#   - NEEDS_RESTORE is armed BEFORE the first byte of launch.sh is
#     modified (and a pristine copy + checksum are pulled before that),
#     so the EXIT trap restores on EVERY path — including a failure
#     partway through injection.
#   - Restore is retried with backoff (short SSH/network outages must not
#     strand the device) and fails LOUDLY (non-zero exit + CRITICAL line
#     with the exact manual recovery command) if the launcher cannot be
#     put back byte-identical. The device must never be left injected.
#   - Refuses to start when a miyoofin is already running, when MainUI is
#     not the sole foreground UI, or when a harness backup is already
#     present (never clobbers; tells the operator how to restore).
#   - MainUI is never stopped/started directly; the Onion helpers own the
#     handoff. Residency is verified after the run (warning on mismatch).
#   - Restore runs on EVERY exit path — success, assertion failure,
#     timeout, INT, TERM — via the EXIT trap, and fails LOUDLY (non-zero
#     exit) if the launcher cannot be put back. The device must never be
#     left with an injected launcher.
#   - Every wait is bounded; the whole run terminates deterministically.

set -eu

DRY_RUN=0
DEVICE_KEYS=1
TIMEOUT_S=${MIYOOFIN_UI_TIMEOUT_S:-300}
NAME=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY_RUN=1; shift ;;
        --desktop-keys) DEVICE_KEYS=0; shift ;;
        --timeout-s) TIMEOUT_S=${2:?--timeout-s needs a value}; shift 2 ;;
        --timeout-s=*) TIMEOUT_S=${1#--timeout-s=}; shift ;;
        -h|--help)
            echo "usage: $0 [--dry-run] [--desktop-keys] [--timeout-s N] <smoke|series>"
            exit 0 ;;
        -*) echo "device-run: unknown flag: $1" >&2; exit 2 ;;
        *) NAME=$1; shift ;;
    esac
done
[ -n "$NAME" ] || { echo "usage: device-run.sh [--dry-run] [--desktop-keys] [--timeout-s N] <smoke|series>" >&2; exit 2; }

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
SCRIPT="$SCRIPT_DIR/scripts/$NAME-device.txt"
OUT="$ROOT/output/ui-script/device-$NAME"
SHIM_SRC="$SCRIPT_DIR/shim.cpp"
SHIM_ARM="$ROOT/output/ui-script/shim-arm.so"
APP_DIR=${MIYOOFIN_DEVICE_APP_DIR:-/mnt/SDCARD/App/MiyooFin}
SCRATCH=/tmp/miyoofin-ui-script
BACKUP=launch.sh.uiscript-bak

[ -f "$SCRIPT" ] || { echo "device-run: no such device script: $SCRIPT" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "device-run: python3 required" >&2; exit 2; }

# shellcheck disable=SC1091
. "$ROOT/tools/miyoo/ssh-common.sh"
# Pure-local launcher surgery shared with the offline round-trip test.
# device-run.sh never reimplements it: the functions below are piped to
# the device through SSH stdin, and the test sources this same file.
# shellcheck disable=SC1091
. "$SCRIPT_DIR/launcher-surgery.sh"
TARGET=$MIYOO_SSH_TARGET

fail() { echo "device-run($NAME): $*" >&2; exit 1; }

# --- dry-run -------------------------------------------------------------
# Prints every host/remote action, executes nothing (no SSH).
if [ "$DRY_RUN" = 1 ]; then
if [ "$DEVICE_KEYS" = 1 ]; then
    DEVKEYS_LINE='export MIYOOFIN_UI_DEVICE_KEYS=1 (KEY names -> Miyoo scancodes)'
else
    DEVKEYS_LINE='unset MIYOOFIN_UI_DEVICE_KEYS (shim sends desktop WASD codes)'
fi
    cat <<EOF
dry-run: target=$TARGET appdir=$APP_DIR script=$SCRIPT timeout=${TIMEOUT_S}s
dry-run: host: make -f tools/ui-script/Makefile.arm
dry-run: ssh $TARGET 'test -x $APP_DIR/launch.sh, refuse if $BACKUP present (restore first), refuse if miyoofin running, require sole MainUI'
dry-run: ssh $TARGET 'mkdir -p $SCRATCH $SCRATCH/shots'  (scp does not create parents)
dry-run: scp $SHIM_ARM -> $TARGET:$SCRATCH/shim-arm.so
dry-run: scp $SCRIPT -> $TARGET:$SCRATCH/script.txt
dry-run: ssh $TARGET 'plain cp (no -p) $APP_DIR/launch.sh $APP_DIR/$BACKUP, cmp backup vs original'  (refuse if backup already there)
dry-run: ssh $TARGET 'build injected content with awk, install IN PLACE via cat > $APP_DIR/launch.sh (no chmod, no mv, no rename — preserves root-owned 0777 inode), then verify: executable bit kept, LD_PRELOAD/MIYOOFIN_UI_* present, sh -n clean'
dry-run:   export LD_PRELOAD=$SCRATCH/shim-arm.so
dry-run:   export MIYOOFIN_UI_SCRIPT=$SCRATCH/script.txt MIYOOFIN_UI_LOG=$SCRATCH/app.log
dry-run:   export MIYOOFIN_UI_SHOT_DIR=$SCRATCH/shots MIYOOFIN_UI_RESULT=$SCRATCH/result.txt
dry-run:   $DEVKEYS_LINE
dry-run:   : > \$MIYOOFIN_UI_LOG; : > \$MIYOOFIN_UI_RESULT
dry-run:   ./miyoofin >>\$MIYOOFIN_UI_LOG 2>&1  (rest of launch.sh unchanged)
dry-run: host: sh tools/miyoo/onion-remote-launch.sh &  (privileged Onion handoff, runs as root like production; MainUI must be resident)
dry-run: host: poll /proc comm loop over SSH for miyoofin (BusyBox pgrep -x is broken), <=120s, else fail
dry-run: host: poll $SCRATCH/result.txt for 'script complete, quitting app' / FAIL every 3s, bound ${TIMEOUT_S}s (uses REAL server/session; no stub, no tunnel)
dry-run: host: if app still resident: sh tools/miyoo/onion-remote-exit.sh (bounded); verify MainUI sole foreground again
dry-run: scp -r $TARGET:$SCRATCH/shots $TARGET:$SCRATCH/app.log $TARGET:$SCRATCH/result.txt -> $OUT/
dry-run: RESTORE (always, every path incl. INT/TERM and inject failure): NEEDS_RESTORE is armed BEFORE launch.sh is first touched; ssh $TARGET 'cat $APP_DIR/$BACKUP > $APP_DIR/launch.sh in place, cmp vs backup, checksum vs pre-injection value, sh -n clean, MIYOOFIN_UI/LD_PRELOAD absent, executable kept, rm backup'; retried with backoff; fail loudly (CRITICAL + manual recovery command) if unverifiable
dry-run: ssh $TARGET 'rm -rf $SCRATCH'; reap launch helper
dry-run: host: grep log marker + python3 tools/ui-script/assert_shots.py (same oracle as run.sh)
EOF
    exit 0
fi

# --- 1. build the ARM shim -----------------------------------------------
make -f "$SCRIPT_DIR/Makefile.arm" >/dev/null || fail "ARM shim build failed"
[ -f "$SHIM_ARM" ] || fail "ARM shim missing after build: $SHIM_ARM"
[ "$SHIM_SRC" -ot "$SHIM_ARM" ] || fail "ARM shim older than $SHIM_SRC"

# --- globals for the cleanup trap ----------------------------------------
RUNDIR=$(mktemp -d /tmp/miyoofin-ui-device.XXXXXX)
LAUNCH_PID=""
NEEDS_RESTORE=0
SCRATCH_PUSHED=0
IN_CLEANUP=0
# Pre-injection pristine state, captured BEFORE NEEDS_RESTORE is armed
# (invariant: NEEDS_RESTORE=1 implies both are populated). ORIG_SUM is the
# on-device checksum the restore must reproduce; launch.orig is the local
# pristine copy the restored file is byte-compared against.
ORIG_SUM=""
RESTORE_ATTEMPTS=4

# Count device processes by comm name over SSH. Prints the count, or -1
# when SSH itself fails (callers treat -1 as "unknown, keep waiting").
# BusyBox pgrep -x is broken on this device, hence the /proc loop.
count_remote() {
    case ${1:?} in *\'*) fail "bad process name" ;; esac
    miyoo_ssh "$TARGET" "n=0; for p in /proc/[0-9]*/comm; do c=\$(cat \"\$p\" 2>/dev/null || true); [ \"\$c\" = '$1' ] && n=\$((n+1)) || true; done; echo \$n" 2>/dev/null || echo -1
}

# The exact shell command that recovers the launcher by hand. It uses only
# operations the unprivileged SSH user is allowed on the root-owned 0777
# file (in-place redirect, no cp -p, no chmod) and re-verifies afterwards.
manual_restore_cmd() {
    # Single-quoted remote command containing only double quotes, so no
    # nested-quote escaping is needed. APP_DIR/BACKUP are operator-set
    # paths without single quotes.
    printf 'ssh -p %s %s %s' "$MIYOO_SSH_PORT" "$TARGET" \
        "'cat \"$APP_DIR/$BACKUP\" > \"$APP_DIR/launch.sh\" && rm \"$APP_DIR/$BACKUP\" && sh -n \"$APP_DIR/launch.sh\" && ! grep -q \"MIYOOFIN_UI_\\|LD_PRELOAD=\" \"$APP_DIR/launch.sh\" && echo RESTORED'"
}

# Run one restore attempt on the device by piping the SHARED surgery
# library through SSH stdin — the device executes the same
# miyoofin_launcher_restore bytes the offline test runs locally.
remote_restore_once() {
    {
        cat "$SCRIPT_DIR/launcher-surgery.sh"
        printf '\nmiyoofin_launcher_restore "$@"\n'
    } | miyoo_ssh "$TARGET" sh -s -- "$APP_DIR" "$BACKUP" "$ORIG_SUM"
}

# The single most important safety property: put the device's launcher
# back byte-identical and PROVE it (in-place rewrite from the backup,
# cmp byte-compare, pre-injection checksum match, injected-marker-absent,
# executable bit kept), then remove the backup. Retried with backoff so a
# transient SSH/network failure cannot strand the device; fails loudly
# with the manual recovery command on any mismatch.
restore_launcher() {
    attempt=1
    delay=2
    while [ "$attempt" -le "$RESTORE_ATTEMPTS" ]; do
        if RESTORE_OUT=$(remote_restore_once 2>&1); then
            printf '%s\n' "$RESTORE_OUT"
            break
        fi
        printf '%s\n' "$RESTORE_OUT" >&2
        if [ "$attempt" -eq "$RESTORE_ATTEMPTS" ]; then
            echo "device-run: CRITICAL: launch.sh restore FAILED after $attempt attempts — the device may still carry an injected launcher." >&2
            echo "device-run: CRITICAL: restore it manually BEFORE launching from Onion, then verify:" >&2
            echo "device-run: CRITICAL: $(manual_restore_cmd)" >&2
            return 1
        fi
        echo "device-run: restore attempt $attempt failed; retrying in ${delay}s" >&2
        sleep "$delay"
        delay=$((delay * 2))
        attempt=$((attempt + 1))
    done
    # Belt-and-braces: byte-compare the restored file against the pristine
    # copy pulled before injection. Best-effort only (the on-device checks
    # above are authoritative) — a dead network must not turn a verified
    # restore into a failure.
    if [ -f "$RUNDIR/launch.orig" ]; then
        if miyoo_scp "$TARGET:$APP_DIR/launch.sh" "$RUNDIR/launch.restored" 2>/dev/null \
            && cmp -s "$RUNDIR/launch.orig" "$RUNDIR/launch.restored"; then
            echo "device-run: restored launcher is byte-identical to the pre-run original"
        else
            echo "device-run: WARNING: could not byte-compare the restored launcher against the pre-run copy (on-device verification above still stands)" >&2
        fi
        rm -f "$RUNDIR/launch.restored"
    fi
    return 0
}

cleanup() {
    rc=$?
    if [ "$IN_CLEANUP" = 1 ]; then exit "$rc"; fi
    IN_CLEANUP=1
    if [ "$NEEDS_RESTORE" = 1 ]; then
        if restore_launcher >"$RUNDIR/restore.log" 2>&1; then
            cat "$RUNDIR/restore.log"
            NEEDS_RESTORE=0
        else
            # restore_launcher already printed the CRITICAL lines plus the
            # manual recovery command; surface them — never swallow a
            # failed restore.
            sed 's/^/restore: /' "$RUNDIR/restore.log" >&2 || true
            echo "device-run: CRITICAL: launcher restore failed (see restore: lines above); do NOT launch from Onion until it is recovered" >&2
            rc=1
        fi
    fi
    if [ "$SCRATCH_PUSHED" = 1 ]; then
        miyoo_ssh "$TARGET" "rm -rf '$SCRATCH'" 2>/dev/null \
            || echo "device-run: WARNING: scratch removal failed ($SCRATCH)" >&2
        SCRATCH_PUSHED=0
    fi
    if [ -n "$LAUNCH_PID" ] && kill -0 "$LAUNCH_PID" 2>/dev/null; then
        kill "$LAUNCH_PID" 2>/dev/null || true
        wait "$LAUNCH_PID" 2>/dev/null || true
    fi
    LAUNCH_PID=""
    rm -rf "$RUNDIR"
    exit "$rc"
}
# EXIT owns all cleanup (restore + scratch + helper reap). INT/TERM convert
# to an exit code first so the EXIT trap sees a failure status.
# NOTE: the app runs as root while this harness is an unprivileged SSH
# user, so the harness cannot signal the app directly (no TERM/KILL
# escalation exists on this path — app exit goes only through
# tools/miyoo/onion-remote-exit.sh). INT/TERM here mean "give up waiting";
# the EXIT trap still restores the launcher first.
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# --- 2. device preconditions ---------------------------------------------
# Refuse early — before pushing anything — when the device is not in the
# expected state. In particular NEVER clobber an existing harness backup:
# it means a previous run failed to restore, and only the operator knows
# whether the installed launcher is safe to overwrite.
miyoo_ssh "$TARGET" sh -s -- "$APP_DIR" "$BACKUP" <<'EOF' || fail "device precondition check failed"
set -eu
appdir=${1:?}; bak=${2:?}
[ -x "$appdir/launch.sh" ] || { echo "device-run: $appdir/launch.sh missing/not executable" >&2; exit 1; }
grep -q '^./miyoofin' "$appdir/launch.sh" \
    || { echo "device-run: $appdir/launch.sh has no ./miyoofin line to inject before" >&2; exit 1; }
if [ -e "$appdir/$bak" ]; then
    echo "device-run: REFUSING: backup $appdir/$bak already present — a previous run did not restore." >&2
    echo "device-run: inspect $appdir/launch.sh, then restore it in place with:" >&2
    echo "device-run:   cat '$appdir/$bak' > '$appdir/launch.sh' && rm '$appdir/$bak' && sh -n '$appdir/launch.sh'" >&2
    echo "device-run: and re-run only once the installed launcher is clean." >&2
    exit 1
fi
n=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = miyoofin ] && n=$((n + 1)) || true
done
[ "$n" -eq 0 ] || { echo "device-run: miyoofin already running; refusing" >&2; exit 1; }
m=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = MainUI ] && m=$((m + 1)) || true
done
[ "$m" -eq 1 ] || { echo "device-run: MainUI is not the sole foreground UI (count=$m); the Onion handoff requires it" >&2; exit 1; }
echo "device-run: preconditions OK (MainUI resident, no miyoofin, no stale backup)"
EOF

# --- 3. push shim + device script ----------------------------------------
# Scratch must exist before the first scp (scp does not create parents).
miyoo_ssh "$TARGET" "mkdir -p '$SCRATCH' '$SCRATCH/shots'" || fail "device mkdir failed"
SCRATCH_PUSHED=1

miyoo_scp "$SHIM_ARM" "$TARGET:$SCRATCH/shim-arm.so" || fail "scp shim failed"
miyoo_scp "$SCRIPT" "$TARGET:$SCRATCH/script.txt" || fail "scp script failed"

# --- 4. pristine copy + backup + inject the launcher --------------------
# Pristine state is captured BEFORE anything is modified (the run has not
# armed NEEDS_RESTORE yet, so a failure here exits with the device
# untouched). ORIG_SUM is the on-device checksum the restore must
# reproduce; launch.orig is the local copy the restored file is
# byte-compared against afterwards.
miyoo_scp "$TARGET:$APP_DIR/launch.sh" "$RUNDIR/launch.orig" \
    || fail "could not pull pristine launch.sh (refusing to inject without a reference copy)"
ORIG_SUM=$( {
    cat "$SCRIPT_DIR/launcher-surgery.sh"
    printf '\nmiyoofin_launcher_sum "$1/launch.sh"\n'
} | miyoo_ssh "$TARGET" sh -s -- "$APP_DIR" ) \
    || fail "could not record pristine launch.sh checksum"
[ -n "$ORIG_SUM" ] || fail "empty pristine launch.sh checksum"
echo "device-run: pristine launch.sh checksum: $ORIG_SUM"

# ARM BEFORE MODIFYING: from here on NEEDS_RESTORE=1, so the EXIT trap
# puts launch.sh back on every path — including a failure partway through
# the injection below (backup taken but install failed) and INT/TERM.
NEEDS_RESTORE=1
{
    cat "$SCRIPT_DIR/launcher-surgery.sh"
    printf '\nmiyoofin_launcher_inject "$@"\n'
} | miyoo_ssh "$TARGET" sh -s -- "$APP_DIR" "$BACKUP" "$SCRATCH" "$DEVICE_KEYS" \
    || fail "launcher injection failed (restore runs on exit)"

# --- 5. privileged launch through Onion ----------------------------------
# onion-remote-launch.sh queues the SAME /tmp/cmd_to_run.sh handoff MainUI
# uses and blocks until the app exits and MainUI returns, so it runs in the
# background while this script polls the shim verdict. The app runs as root
# with its REAL session and real server — no stub, no tunnel.
echo "device-run: triggering privileged Onion launch (as root, real server/session)..."
sh "$ROOT/tools/miyoo/onion-remote-launch.sh" >"$RUNDIR/launch.log" 2>&1 &
LAUNCH_PID=$!

APPEAR_TIMEOUT=$TIMEOUT_S
if [ "$APPEAR_TIMEOUT" -gt 120 ]; then APPEAR_TIMEOUT=120; fi
elapsed=0
APPEARED=0
while [ "$elapsed" -lt "$APPEAR_TIMEOUT" ]; do
    c=$(count_remote miyoofin)
    if [ "$c" != 0 ] && [ "$c" != -1 ]; then APPEARED=1; break; fi
    if ! kill -0 "$LAUNCH_PID" 2>/dev/null; then
        sed 's/^/launch-helper: /' "$RUNDIR/launch.log" >&2 || true
        fail "Onion launch helper exited before the app appeared (see launch-helper lines above)"
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
[ "$APPEARED" = 1 ] || fail "miyoofin did not appear within ${APPEAR_TIMEOUT}s"
echo "device-run: app is resident under Onion (privileged)"

# --- 6. bounded verdict wait ---------------------------------------------
# The device script ends with QUIT (clean exit). Anything else is a hang.
elapsed=0
VERDICT=""
while [ "$elapsed" -lt "$TIMEOUT_S" ]; do
    RES=$(miyoo_ssh "$TARGET" "cat '$SCRATCH/result.txt' 2>/dev/null" 2>/dev/null || true)
    case "$RES" in
        *quitting\ app*) VERDICT=done; break ;;
        *FAIL*) VERDICT=scriptfail; break ;;
    esac
    if [ "$(count_remote miyoofin)" = 0 ]; then VERDICT=exited; break; fi
    sleep 3
    elapsed=$((elapsed + 3))
done
[ -n "$VERDICT" ] || VERDICT=timeout
echo "device-run: verdict wait finished: $VERDICT after ~${elapsed}s"

# --- 7. exit the app ------------------------------------------------------
# QUIT usually already exited it; only call the graceful-exit helper when
# the app is still resident. The helper is the only way out: a non-root SSH
# user cannot signal the root-run app, so TERM/KILL escalation is not
# available on this path. If the helper fails, the operator must exit the
# app physically — but the launcher is still restored below regardless.
if [ "$(count_remote miyoofin)" != 0 ]; then
    echo "device-run: app still resident ($VERDICT) — exiting via onion-remote-exit.sh"
    if ! sh "$ROOT/tools/miyoo/onion-remote-exit.sh" >"$RUNDIR/exit.log" 2>&1; then
        sed 's/^/remote-exit: /' "$RUNDIR/exit.log" >&2 || true
        echo "device-run: WARNING: graceful exit failed; exit the app physically if still running (launcher restore continues)" >&2
    fi
else
    echo "device-run: app already exited"
fi
i=0
while [ "$(count_remote MainUI)" != 1 ] && [ "$i" -lt 45 ]; do
    sleep 1
    i=$((i + 1))
done
[ "$(count_remote MainUI)" = 1 ] \
    || echo "device-run: WARNING: MainUI is not the sole foreground UI after exit" >&2

# Reap the launch helper (it exits once MainUI returns); bounded, then kill.
i=0
while kill -0 "$LAUNCH_PID" 2>/dev/null && [ "$i" -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done
if kill -0 "$LAUNCH_PID" 2>/dev/null; then
    echo "device-run: WARNING: launch helper still alive; killing local client (device state already verified above)" >&2
    kill "$LAUNCH_PID" 2>/dev/null || true
    wait "$LAUNCH_PID" 2>/dev/null || true
else
    wait "$LAUNCH_PID" 2>/dev/null \
        || echo "device-run: WARNING: launch helper exited non-zero (see $RUNDIR/launch.log copy in $OUT)" >&2
fi
LAUNCH_PID=""

# --- 8. pull + restore BEFORE any local verdict ---------------------------
# Scratch is pulled first (restore needs only the app dir), then the
# launcher is restored explicitly so even an assertion failure below cannot
# leave the device injected (the EXIT trap is belt-and-braces after this).
sleep 2
mkdir -p "$OUT/shots"
miyoo_scp -r "$TARGET:$SCRATCH/shots/." "$OUT/shots/" || fail "pull shots failed"
miyoo_scp "$TARGET:$SCRATCH/app.log" "$OUT/app.log" || fail "pull app.log failed"
miyoo_scp "$TARGET:$SCRATCH/result.txt" "$OUT/result.txt" || fail "pull result.txt failed"
cp "$RUNDIR/launch.log" "$OUT/launch.log" 2>/dev/null || true

restore_launcher || fail "launcher restore failed (see CRITICAL lines above for the manual recovery command)"
NEEDS_RESTORE=0

miyoo_ssh "$TARGET" "rm -rf '$SCRATCH'" || echo "device-run: WARNING: scratch removal failed ($SCRATCH)" >&2
SCRATCH_PUSHED=0

# --- 9. assertions (SAME oracle as the desktop runner) ---------------------
if [ "$VERDICT" = timeout ]; then
    fail "app did not reach a verdict within ${TIMEOUT_S}s (script QUIT missing?)"
fi
if grep -q '^FAIL' "$OUT/result.txt" 2>/dev/null; then
    grep '^FAIL' "$OUT/result.txt" | sed 's/^/device-run: /' >&2
    fail "script driver reported failure"
fi
grep -q 'script complete, quitting app' "$OUT/result.txt" 2>/dev/null \
    || fail "no clean script verdict (app $VERDICT; see $OUT/app.log)"

case "$NAME" in
    smoke)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/home.bmp"
        CHECKS="rendered,rails"
        ;;
    series)
        want='[SeriesScreen] enter series='
        SHOT="$OUT/shots/series-seasons.bmp"
        CHECKS="rendered,seasons"
        ;;
    restore-e3d)
        want='[EpisodeBrowserScreen]'
        SHOT="$OUT/shots/restore-e3-final.bmp"
        CHECKS="rendered"
        ;;
    restore-e3c)
        want='[EpisodeBrowserScreen]'
        SHOT="$OUT/shots/restore-episodes.bmp"
        CHECKS="rendered"
        ;;
    restore-e3b)
        want='[SeriesScreen] enter series='
        SHOT="$OUT/shots/restore-seasons.bmp"
        CHECKS="rendered,seasons"
        ;;
    restore-e3)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/restore-show-selected.bmp"
        CHECKS="rendered"
        ;;
    ota-live)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/ota-live.bmp"
        CHECKS="rendered"
        ;;
    offline)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/offline-on.bmp"
        CHECKS="rendered"
        ;;
    dl-lifecycle)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/lc-deleted.bmp"
        CHECKS="rendered"
        ;;
    dl-start)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/dl-back-to-grid.bmp"
        CHECKS="rendered"
        ;;
    dl-pause)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/dl-paused.bmp"
        CHECKS="rendered"
        ;;
    shows-grid)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/shows-grid.bmp"
        CHECKS="rendered"
        ;;
    dl-delete)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/dl-after-delete.bmp"
        CHECKS="rendered"
        ;;
    dlview)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/downloads-tab.bmp"
        CHECKS="rendered"
        ;;
    download)
        # No screenshot steps: the download store on disk is the verification.
        want='[HomeScreen] Library loaded'
        SHOT=""
        CHECKS=""
        ;;
    movies)
        want='[MovieDetailsScreen]'
        SHOT="$OUT/shots/movie-details-download.bmp"
        CHECKS="rendered"
        ;;
    ota)
        # The updater logs nothing, so the meaningful assertion here is that
        # the script reached the UPDATES row and the screenshots rendered; the
        # real verification (installed file modes / mtimes) is done post-run.
        want='[HomeScreen] Library loaded'
        # The install rewrites the app; asserting on the (proven rendered)
        # "checked" screen keeps this check meaningful, and the install itself
        # is verified post-run from the installed file modes.
        SHOT="$OUT/shots/updates-checked.bmp"
        CHECKS="rendered"
        ;;
    *) fail "no expectations defined for script '$NAME'" ;;
esac

grep -Fq "$want" "$OUT/app.log" \
    || fail "expected screen marker missing from log: $want"
echo "device-run: log marker OK: $want"

if [ -n "$SHOT" ]; then
    [ -f "$SHOT" ] || fail "expected screenshot missing: $SHOT"
    python3 "$SCRIPT_DIR/assert_shots.py" --shot "$SHOT" --checks "$CHECKS" \
        || fail "screenshot assertions failed for $SHOT"
else
    echo "device-run: no screenshot assertion for this script (by design)"
fi

echo "device-run($NAME): PASS (shots in $OUT/shots, log in $OUT/app.log)"
