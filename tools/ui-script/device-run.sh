#!/bin/sh
# MiyooFin device UI-harness runner (test-only, ARM/Miyoo Mini Plus).
#
# Drives the REAL installed ARM binary on the Miyoo over SSH with the same
# scripted-input timeline the desktop harness uses:
#   1. builds the ARM LD_PRELOAD input shim (tools/ui-script/Makefile.arm),
#   2. starts the stub Jellyfin on the HOST bound to the LAN so the device
#      can reach it (stub_server.py <portfile> 0.0.0.0),
#   3. pushes the shim + script to a /tmp scratch dir on the device and
#      seeds an ISOLATED runtime cwd there (session.txt only),
#   4. launches the installed miyoofin with LD_PRELOAD=<shim> plus the env
#      the packaged launcher sets (SDL_VIDEODRIVER/AUDIODRIVER=mmiyoo,
#      LD_LIBRARY_PATH=<app>/lib),
#   5. the shim replays KEYs as DEVICE scancodes and captures SCREENSHOTs
#      through the same /tmp flag + app framebuffer hook the desktop
#      harness and tools/miyoo/miyoofin-screenshot.sh use,
#   6. pulls shots + app log back and runs the SAME assertions
#      (tools/ui-script/assert_shots.py) as the desktop variant.
#
# Usage: sh tools/ui-script/device-run.sh [--dry-run] [--desktop-keys]
#            [--timeout-s N] <smoke|series>
#   --dry-run       print every host/remote action, do nothing (no SSH).
#   --desktop-keys  do NOT set MIYOOFIN_UI_DEVICE_KEYS (shim sends WASD
#                   codes; only useful with Raw: scancodes in the script).
# Env:
#   MIYOO_SSH_TARGET / MIYOO_HOST / MIYOO_SSH_PORT (tools/miyoo/ssh-common.sh)
#   MIYOOFIN_UI_HOST_IP   host LAN IP the device uses to reach the stub;
#                         auto-detected via `ip route get` when unset.
#   MIYOOFIN_UI_TIMEOUT_S app-run watchdog seconds (default 180).
#   MIYOOFIN_DEVICE_APP_DIR device app dir (default /mnt/SDCARD/App/MiyooFin).
#
# SAFETY (unattended hardware — read before extending):
#   - NEVER reboots or power-cycles the device (no such command exists here).
#   - NEVER writes outside /tmp/miyoofin-ui-script on the device; the
#     user's downloads, cache, catalog and session files are untouched
#     (the app runs with cwd=scratch, so cwd-relative state lands there).
#   - Refuses to start when a miyoofin is already running.
#   - MainUI is never stopped/started; its residency is recorded at entry
#     and verified unchanged at exit.
#   - Cleanup (trap on EXIT/INT/TERM): graceful exit via the SIGUSR1 helper
#     when present, else SIGTERM, else SIGKILL — bounded waits throughout —
#     then scratch removal. No orphaned miyoofin, no device left running it.
#   - Every wait is bounded; the whole run terminates deterministically.

set -eu

DRY_RUN=0
DEVICE_KEYS=1
TIMEOUT_S=${MIYOOFIN_UI_TIMEOUT_S:-180}
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
SCRIPT="$SCRIPT_DIR/scripts/$NAME.txt"
OUT="$ROOT/output/ui-script/device-$NAME"
SHIM_SRC="$SCRIPT_DIR/shim.cpp"
SHIM_ARM="$ROOT/output/ui-script/shim-arm.so"
APP_DIR=${MIYOOFIN_DEVICE_APP_DIR:-/mnt/SDCARD/App/MiyooFin}
SCRATCH=/tmp/miyoofin-ui-script

[ -f "$SCRIPT" ] || { echo "device-run: no such script: $SCRIPT" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "device-run: python3 required" >&2; exit 2; }

# shellcheck disable=SC1091
. "$ROOT/tools/miyoo/ssh-common.sh"
TARGET=$MIYOO_SSH_TARGET

fail() { echo "device-run($NAME): $*" >&2; exit 1; }

# --- dry-run -------------------------------------------------------------
# Prints every host/remote action, executes nothing (no SSH, no stub).
if [ "$DRY_RUN" = 1 ]; then
if [ "$DEVICE_KEYS" = 1 ]; then
    DEVKEYS_LINE='export MIYOOFIN_UI_DEVICE_KEYS=1 (KEY names -> Miyoo scancodes)'
else
    DEVKEYS_LINE='MIYOOFIN_UI_DEVICE_KEYS unset (shim sends desktop WASD codes)'
fi
    cat <<EOF
dry-run: target=$TARGET app=$APP_DIR/miyoofin script=$SCRIPT timeout=${TIMEOUT_S}s
dry-run: host: make -f tools/ui-script/Makefile.arm
dry-run: host: python3 tools/ui-script/stub_server.py <portfile> 0.0.0.0 &
dry-run: host: wait <=10s for <portfile>, else fail "stub server did not report a port"
dry-run: host: detect host LAN IP (MIYOOFIN_UI_HOST_IP) via: ip route get <device-ip>
dry-run: ssh $TARGET 'test -x $APP_DIR/miyoofin, refuse if miyoofin running, print MainUI count'
dry-run: scp $SHIM_ARM -> $TARGET:$SCRATCH/shim-arm.so
dry-run: scp $SCRIPT -> $TARGET:$SCRATCH/script.txt
dry-run: ssh $TARGET 'mkdir -p $SCRATCH/run $SCRATCH/shots'
dry-run: scp <seeded session.txt server_url=http://<host-ip>:<port>> -> $TARGET:$SCRATCH/run/session.txt
dry-run: ssh $TARGET 'sh -s' with args ($SCRATCH, $APP_DIR, $DEVICE_KEYS) <<'REMOTE'
dry-run:   : > \$scratch/app.log; : > \$scratch/result.txt; cd \$scratch/run
dry-run:   $DEVKEYS_LINE
dry-run:   export SDL_VIDEODRIVER=mmiyoo SDL_AUDIODRIVER=mmiyoo
dry-run:   export LD_LIBRARY_PATH=\$appdir/lib LD_PRELOAD=\$scratch/shim-arm.so
dry-run:   export MIYOOFIN_UI_SCRIPT=\$scratch/script.txt MIYOOFIN_UI_LOG=\$scratch/app.log
dry-run:   export MIYOOFIN_UI_SHOT_DIR=\$scratch/shots MIYOOFIN_UI_RESULT=\$scratch/result.txt
dry-run:   ("\$appdir/miyoofin" >>\$scratch/app.log 2>&1 & echo \$!)  # prints remote pid
dry-run: host: poll 'kill -0 <pid>' over SSH every 2s, bound ${TIMEOUT_S}s, else kill + fail
dry-run: scp -r $TARGET:$SCRATCH/shots $TARGET:$SCRATCH/app.log $TARGET:$SCRATCH/result.txt -> $OUT/
dry-run: host: grep log marker + python3 tools/ui-script/assert_shots.py (same oracle as run.sh)
dry-run: cleanup (always): graceful-exit helper else TERM else KILL (<=15s), verify no miyoofin + MainUI unchanged, rm -rf $SCRATCH, kill stub
EOF
    exit 0
fi

# --- 1. build the ARM shim -----------------------------------------------
make -f "$SCRIPT_DIR/Makefile.arm" >/dev/null || fail "ARM shim build failed"
[ -f "$SHIM_ARM" ] || fail "ARM shim missing after build: $SHIM_ARM"
[ "$SHIM_SRC" -ot "$SHIM_ARM" ] || fail "ARM shim older than $SHIM_SRC"

# --- globals for the cleanup trap ----------------------------------------
RUNDIR=$(mktemp -d /tmp/miyoofin-ui-device.XXXXXX)
STUB_PID=""
REMOTE_PID=""
MAINUI_BEFORE=""

remote_cleanup() {
    # Graceful first (same SIGUSR1 helper onion-remote-exit.sh uses),
    # then TERM, then KILL — every step bounded. Never touches MainUI,
    # never reboots, never touches user data (scratch only).
    miyoo_ssh "$TARGET" sh -s -- "$SCRATCH" "$REMOTE_PID" "$MAINUI_BEFORE" <<'EOF' || true
set -u
scratch=$1; pid=$2; mainui_before=$3
if [ -n "$pid" ] && [ -d /proc/"$pid" ] 2>/dev/null; then
    if [ -x /tmp/miyoofin-graceful-exit ]; then
        /tmp/miyoofin-graceful-exit 2>/dev/null || true
    else
        kill "$pid" 2>/dev/null || true
    fi
    n=0
    while [ -d /proc/"$pid" ] 2>/dev/null && [ "$n" -lt 15 ]; do
        sleep 1; n=$((n + 1))
    done
    if [ -d /proc/"$pid" ] 2>/dev/null; then
        kill -9 "$pid" 2>/dev/null || true
        sleep 1
    fi
    [ -d /proc/"$pid" ] 2>/dev/null \
        && echo "device-run: WARNING: app pid $pid still present after KILL" >&2 || true
fi
left=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = miyoofin ] && left=$((left + 1)) || true
done
[ "$left" -eq 0 ] || echo "device-run: WARNING: $left miyoofin process(es) remain" >&2
now=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = MainUI ] && now=$((now + 1)) || true
done
[ "$now" = "$mainui_before" ] \
    || echo "device-run: WARNING: MainUI residency changed ($mainui_before -> $now)" >&2
rm -rf "$scratch"
EOF
    REMOTE_PID=""
}

cleanup() {
    # Always leave the device without our app running and without scratch.
    if [ -n "$REMOTE_PID" ] || [ -n "$MAINUI_BEFORE" ]; then
        remote_cleanup || true
        MAINUI_BEFORE=""
    fi
    if [ -n "$STUB_PID" ] && kill -0 "$STUB_PID" 2>/dev/null; then
        kill "$STUB_PID" 2>/dev/null || true
        wait "$STUB_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNDIR"
}
trap cleanup EXIT INT TERM

# --- 2. stub server on the host LAN --------------------------------------
python3 "$SCRIPT_DIR/stub_server.py" "$RUNDIR/port" 0.0.0.0 >"$RUNDIR/stub.log" 2>&1 &
STUB_PID=$!
PORT=""
i=0
while [ $i -lt 100 ]; do
    if [ -s "$RUNDIR/port" ]; then PORT=$(cat "$RUNDIR/port"); break; fi
    if ! kill -0 "$STUB_PID" 2>/dev/null; then
        sed 's/^/stub: /' "$RUNDIR/stub.log" >&2 || true
        fail "stub server died on startup"
    fi
    sleep 0.1
    i=$((i + 1))
done
[ -n "$PORT" ] || fail "stub server did not report a port"
echo "device-run: stub Jellyfin on 0.0.0.0:$PORT"

# Host LAN IP the device must use to reach this stub.
if [ -n "${MIYOOFIN_UI_HOST_IP:-}" ]; then
    HOST_IP=$MIYOOFIN_UI_HOST_IP
else
    DEV_IP=${TARGET##*@}
    HOST_IP=$(ip route get "$DEV_IP" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -1 || true)
    [ -n "$HOST_IP" ] || fail "cannot detect host LAN IP (no route to $DEV_IP); export MIYOOFIN_UI_HOST_IP"
fi
echo "device-run: device reaches stub via http://$HOST_IP:$PORT"

# --- 3. device preconditions + push --------------------------------------
# Refuse when an app instance is already running; record MainUI residency
# so cleanup can verify it is unchanged (never restarted by us).
MAINUI_BEFORE=$(miyoo_ssh "$TARGET" sh -s -- "$APP_DIR" <<'EOF' || fail "device precondition check failed (ssh?)"
set -eu
appdir=$1
[ -x "$appdir/miyoofin" ] || { echo "device-run: $appdir/miyoofin missing/not executable" >&2; exit 1; }
[ -d "$appdir/lib" ] || { echo "device-run: $appdir/lib missing" >&2; exit 1; }
n=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = miyoofin ] && n=$((n + 1)) || true
done
[ "$n" -eq 0 ] || { echo "device-run: miyoofin already running; refusing" >&2; exit 1; }
n=0
for proc in /proc/[0-9]*/comm; do
    [ "$(cat "$proc" 2>/dev/null || true)" = MainUI ] && n=$((n + 1)) || true
done
printf '%s' "$n"
EOF
)
echo "device-run: preconditions OK (MainUI resident count: $MAINUI_BEFORE)"

miyoo_scp "$SHIM_ARM" "$TARGET:$SCRATCH/shim-arm.so" || fail "scp shim failed"
miyoo_scp "$SCRIPT" "$TARGET:$SCRATCH/script.txt" || fail "scp script failed"

# Seeded session: stub token/user against the HOST stub over the LAN.
# Written only into the isolated scratch cwd — never the real session.
miyoo_ssh "$TARGET" "mkdir -p '$SCRATCH/run' '$SCRATCH/shots'" || fail "device mkdir failed"
cat >"$RUNDIR/session.txt" <<EOF
server_url=http://$HOST_IP:$PORT
access_token=stub-token
user_id=user-stub
user_name=stub
manual_offline_mode=0
EOF
miyoo_scp "$RUNDIR/session.txt" "$TARGET:$SCRATCH/run/session.txt" || fail "scp session failed"
miyoo_ssh "$TARGET" "chmod 600 '$SCRATCH/run/session.txt'" || fail "device chmod failed"

# --- 4. launch the app under the shim ------------------------------------
# Same env the packaged launcher sets, plus the harness env. cwd is the
# scratch run dir so all cwd-relative state (session, cache, downloads)
# stays isolated from the user's real data. MIYOOFIN_DESKTOP_INPUT is
# deliberately NOT set: the app uses its device input mapping, and the
# shim sends device scancodes (MIYOOFIN_UI_DEVICE_KEYS=1) unless
# --desktop-keys was given.
REMOTE_PID=$(miyoo_ssh "$TARGET" sh -s -- "$SCRATCH" "$APP_DIR" "$DEVICE_KEYS" <<'EOF' || fail "device launch failed"
set -eu
scratch=$1; appdir=$2; devicekeys=$3
: > "$scratch/app.log"
: > "$scratch/result.txt"
cd "$scratch/run"
if [ "$devicekeys" = 1 ]; then export MIYOOFIN_UI_DEVICE_KEYS=1; else unset MIYOOFIN_UI_DEVICE_KEYS; fi
export SDL_VIDEODRIVER=mmiyoo
export SDL_AUDIODRIVER=mmiyoo
export LD_LIBRARY_PATH="$appdir/lib"
export LD_PRELOAD="$scratch/shim-arm.so"
export MIYOOFIN_UI_SCRIPT="$scratch/script.txt"
export MIYOOFIN_UI_LOG="$scratch/app.log"
export MIYOOFIN_UI_SHOT_DIR="$scratch/shots"
export MIYOOFIN_UI_RESULT="$scratch/result.txt"
# Detached reparent: output redirected, so this SSH session ends while
# the app keeps running under the shim's scripted timeline. The echoed
# pid lets the host watchdog and cleanup address exactly this instance.
("$appdir/miyoofin" >>"$scratch/app.log" 2>&1 & echo $!)
EOF
)
case "$REMOTE_PID" in
    ''|*[!0-9]*) fail "device launch returned bad pid: $REMOTE_PID" ;;
esac
echo "device-run: app launched as remote pid $REMOTE_PID"

# --- 5. bounded watchdog ---------------------------------------------------
# The script ends with QUIT (clean exit). Anything else is a hang — the
# trap's remote_cleanup kills it and MainUI state is verified after.
elapsed=0
while miyoo_ssh "$TARGET" "kill -0 '$REMOTE_PID' 2>/dev/null"; do
    if [ "$elapsed" -ge "$TIMEOUT_S" ]; then
        # remote_cleanup kills the hung app (TERM/KILL escalation),
        # verifies device state, and clears REMOTE_PID.
        remote_cleanup || true
        MAINUI_BEFORE=""
        fail "app did not exit within ${TIMEOUT_S}s (script QUIT missing?)"
    fi
    sleep 2
    elapsed=$((elapsed + 2))
done
echo "device-run: app exited after ~${elapsed}s"
# Give the shim's last SCREENSHOT a moment to flush, then pull everything.
sleep 2
mkdir -p "$OUT/shots"
miyoo_scp -r "$TARGET:$SCRATCH/shots/." "$OUT/shots/" || fail "pull shots failed"
miyoo_scp "$TARGET:$SCRATCH/app.log" "$OUT/app.log" || fail "pull app.log failed"
miyoo_scp "$TARGET:$SCRATCH/result.txt" "$OUT/result.txt" || fail "pull result.txt failed"

# --- 6. device cleanup before local verdict -------------------------------
# Scratch is removed now so even an assertion failure below cannot leave
# device state behind (the EXIT trap is belt-and-braces after this).
remote_cleanup || fail "device cleanup failed"
MAINUI_BEFORE=""
cp "$RUNDIR/stub.log" "$OUT/stub.log" 2>/dev/null || true

# --- 7. assertions (SAME oracle as the desktop runner) ---------------------
# Expectations below mirror run.sh exactly; keep the two in sync.
if grep -q '^FAIL' "$OUT/result.txt" 2>/dev/null; then
    grep '^FAIL' "$OUT/result.txt" | sed 's/^/device-run: /' >&2
    fail "script driver reported failure"
fi

case "$NAME" in
    smoke)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/home.bmp"
        CHECKS="rendered,rails"
        ;;
    series)
        want='[SeriesScreen] enter series=Testville'
        SHOT="$OUT/shots/series-seasons.bmp"
        CHECKS="rendered,seasons"
        ;;
    *) fail "no expectations defined for script '$NAME'" ;;
esac

grep -Fq "$want" "$OUT/app.log" \
    || fail "expected screen marker missing from log: $want"
echo "device-run: log marker OK: $want"

[ -f "$SHOT" ] || fail "expected screenshot missing: $SHOT"
python3 "$SCRIPT_DIR/assert_shots.py" --shot "$SHOT" --checks "$CHECKS" \
    || fail "screenshot assertions failed for $SHOT"

echo "device-run($NAME): PASS (shots in $OUT/shots, log in $OUT/app.log)"
