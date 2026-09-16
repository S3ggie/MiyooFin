#!/bin/sh
# Offline inject/restore round-trip test for the ui-script device harness.
#
# No device, no SSH, no network: sources
# tools/ui-script/launcher-surgery.sh (the EXACT bytes device-run.sh pipes
# to the device through SSH stdin) and runs the inject/restore functions
# against a local fixture directory seeded with the real packaged
# distributions/onionos/launch.sh at a realistic mode 0777.
#
# Proves:
#   T1  happy path: inject succeeds (executable, valid, markers present,
#       mode+inode preserved = written in place), restore returns the file
#       BYTE-IDENTICAL (checksum match), backup removed, verification clean.
#   T1b devicekeys=0 variant installs the unset line instead of the export.
#   T2  failure path: injection failing halfway (backup taken, install
#       blocked — the shape of the real-hardware incident) still restores
#       byte-identically, and the inject step exits non-zero.
#   T3  stale-backup refusal: a second inject refuses and leaves the file
#       untouched; the following restore is still byte-identical.
#   T4  restore with no backup and a clean launcher succeeds (nothing to do).
#   T5  restore with no backup and an injected launcher fails loudly.
#   T6  static safety invariants on device-run.sh itself: NEEDS_RESTORE is
#       armed textually BEFORE the injection SSH call, EXIT/INT/TERM traps
#       exist, and no executable cp -p/chmod/mv remains in either file.
#   T7  `sh -n` passes on every harness script.
#   T8  `device-run.sh --dry-run` runs offline and describes the in-place
#       mechanism.
#
# Usage: sh tools/ui-script/test-launcher-roundtrip.sh
#   (also runs as the first step of `make ui-script-test`).

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
LIB="$SCRIPT_DIR/launcher-surgery.sh"
REAL_LAUNCH="$ROOT/distributions/onionos/launch.sh"

# shellcheck disable=SC1091
. "$LIB"

PASS=0
FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok: $1"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL: $1" >&2; }

[ -f "$REAL_LAUNCH" ] || { echo "FAIL: packaged launcher missing: $REAL_LAUNCH" >&2; exit 1; }

# Build a fixture dir: real launch.sh content at realistic mode 0777.
new_fixture() {
    _fix=$(mktemp -d /tmp/uiscript-fixture.XXXXXX)
    cp "$REAL_LAUNCH" "$_fix/launch.sh"
    chmod 777 "$_fix/launch.sh"
    printf '%s' "$_fix"
}

# file_stat <file> -> "mode inode" (GNU/BusyBox stat); empty when stat -c
# is unavailable, in which case mode/inode assertions are skipped.
file_stat() {
    if stat -c '%a %i' "${1:?}" 2>/dev/null; then return 0; fi
    echo ""
}

echo "--- T1: happy-path round trip (byte-identical restore) ---"
FIX=$(new_fixture)
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
STAT0=$(file_stat "$FIX/launch.sh")
BAK=launch.sh.uiscript-bak
if miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1; then
    ok "T1 inject exits 0"
else
    bad "T1 inject exits 0"
fi
[ -x "$FIX/launch.sh" ] && ok "T1 injected file executable" || bad "T1 injected file executable"
sh -n "$FIX/launch.sh" 2>/dev/null && ok "T1 injected file passes sh -n" || bad "T1 injected file passes sh -n"
grep -q 'MIYOOFIN_UI_SCRIPT' "$FIX/launch.sh" && ok "T1 shim exports present" || bad "T1 shim exports present"
grep -q 'LD_PRELOAD=' "$FIX/launch.sh" && ok "T1 LD_PRELOAD present" || bad "T1 LD_PRELOAD present"
[ -f "$FIX/$BAK" ] && ok "T1 backup present after inject" || bad "T1 backup present after inject"
[ ! -e "$FIX/launch.sh.uiscript-new" ] && ok "T1 temp file cleaned up" || bad "T1 temp file cleaned up"
if [ -n "$STAT0" ]; then
    if [ "$(file_stat "$FIX/launch.sh")" = "$STAT0" ]; then
        ok "T1 mode+inode preserved (written in place, no chmod/mv)"
    else
        bad "T1 mode+inode preserved (got $(file_stat "$FIX/launch.sh"), want $STAT0)"
    fi
else
    echo "skip: T1 mode/inode check (no stat -c)"
fi
if miyoofin_launcher_restore "$FIX" "$BAK" "$SUM0" >/dev/null 2>&1; then
    ok "T1 restore exits 0"
else
    bad "T1 restore exits 0"
fi
if [ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ]; then
    ok "T1 restored checksum $SUM0 matches original (byte-identical)"
else
    bad "T1 restored checksum matches original"
fi
[ ! -e "$FIX/$BAK" ] && ok "T1 backup removed" || bad "T1 backup removed"
if miyoofin_launcher_check_clean "$FIX/launch.sh" >/dev/null 2>&1; then
    ok "T1 restored file verifies clean"
else
    bad "T1 restored file verifies clean"
fi
if [ -n "$STAT0" ]; then
    if [ "$(file_stat "$FIX/launch.sh")" = "$STAT0" ]; then
        ok "T1 mode+inode still preserved after restore"
    else
        bad "T1 mode+inode still preserved after restore"
    fi
fi
rm -rf "$FIX"

echo "--- T1b: --desktop-keys variant (devicekeys=0) ---"
FIX=$(new_fixture)
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
if miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 0 >/dev/null 2>&1; then
    ok "T1b inject exits 0"
else
    bad "T1b inject exits 0"
fi
grep -q 'unset MIYOOFIN_UI_DEVICE_KEYS' "$FIX/launch.sh" \
    && ok "T1b unset line installed" || bad "T1b unset line installed"
! grep -q 'MIYOOFIN_UI_DEVICE_KEYS=1' "$FIX/launch.sh" \
    && ok "T1b no device-keys export" || bad "T1b no device-keys export"
if miyoofin_launcher_restore "$FIX" "$BAK" "$SUM0" >/dev/null 2>&1 \
    && [ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ]; then
    ok "T1b restore byte-identical"
else
    bad "T1b restore byte-identical"
fi
rm -rf "$FIX"

echo "--- T2: inject fails halfway, trap-path restore still recovers ---"
FIX=$(new_fixture)
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
# Block the build step the way the hardware failure blocked install: the
# temp path exists as a directory, so the awk redirect cannot create it.
# The backup is already taken at that point — exactly the stranded state.
mkdir "$FIX/launch.sh.uiscript-new"
if miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1; then
    bad "T2 halfway inject exits non-zero"
else
    ok "T2 halfway inject exits non-zero"
fi
[ -f "$FIX/$BAK" ] && ok "T2 backup exists after failed inject (halfway state)" \
    || bad "T2 backup exists after failed inject (halfway state)"
if [ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ]; then
    ok "T2 launch.sh untouched by the failed inject"
else
    bad "T2 launch.sh untouched by the failed inject"
fi
# This is what the EXIT trap runs (NEEDS_RESTORE was armed before the
# inject call): it must recover byte-identically.
if miyoofin_launcher_restore "$FIX" "$BAK" "$SUM0" >/dev/null 2>&1; then
    ok "T2 trap-path restore exits 0"
else
    bad "T2 trap-path restore exits 0"
fi
if [ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ] && [ ! -e "$FIX/$BAK" ]; then
    ok "T2 restored byte-identical ($SUM0), backup gone"
else
    bad "T2 restored byte-identical, backup gone"
fi
rm -rf "$FIX"

echo "--- T3: stale-backup refusal ---"
FIX=$(new_fixture)
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1
SUM_INJ=$(miyoofin_launcher_sum "$FIX/launch.sh")
if miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1; then
    bad "T3 second inject refuses (non-zero)"
else
    ok "T3 second inject refuses (non-zero)"
fi
[ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM_INJ" ] \
    && ok "T3 refused inject leaves file untouched" || bad "T3 refused inject leaves file untouched"
if miyoofin_launcher_restore "$FIX" "$BAK" "$SUM0" >/dev/null 2>&1 \
    && [ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ]; then
    ok "T3 restore after refusal byte-identical"
else
    bad "T3 restore after refusal byte-identical"
fi
rm -rf "$FIX"

echo "--- T4/T5: restore with no backup ---"
FIX=$(new_fixture)
if miyoofin_launcher_restore "$FIX" "$BAK" >/dev/null 2>&1; then
    ok "T4 clean launcher + no backup: restore succeeds (nothing to do)"
else
    bad "T4 clean launcher + no backup: restore succeeds (nothing to do)"
fi
printf '%s\n' 'export LD_PRELOAD=/tmp/rogue.so' >>"$FIX/launch.sh"
if miyoofin_launcher_restore "$FIX" "$BAK" >/dev/null 2>&1; then
    bad "T5 injected launcher + no backup: restore fails loudly"
else
    ok "T5 injected launcher + no backup: restore fails loudly"
fi
rm -rf "$FIX"

echo "--- T6: static safety invariants on device-run.sh ---"
DR=device-run.sh
ARM_LINE=$(grep -n '^NEEDS_RESTORE=1' "$SCRIPT_DIR/$DR" | head -1 | cut -d: -f1)
INJ_LINE=$(grep -n 'miyoofin_launcher_inject' "$SCRIPT_DIR/$DR" | head -1 | cut -d: -f1)
if [ -n "$ARM_LINE" ] && [ -n "$INJ_LINE" ] && [ "$ARM_LINE" -lt "$INJ_LINE" ]; then
    ok "T6 NEEDS_RESTORE armed (line $ARM_LINE) before inject call (line $INJ_LINE)"
else
    bad "T6 NEEDS_RESTORE armed before inject call (arm=$ARM_LINE inject=$INJ_LINE)"
fi
for sig in 'trap cleanup EXIT' "trap 'exit 130' INT" "trap 'exit 143' TERM"; do
    grep -qF "$sig" "$SCRIPT_DIR/$DR" && ok "T6 trap present: $sig" || bad "T6 trap present: $sig"
done
if grep -nE '^[[:space:]]*(cp +-p|chmod([[:space:]]|$)|mv )' "$SCRIPT_DIR/$DR" "$LIB" >/dev/null 2>&1; then
    bad "T6 no executable cp -p/chmod/mv in harness"
else
    ok "T6 no executable cp -p/chmod/mv in harness"
fi
grep -q 'manual_restore_cmd' "$SCRIPT_DIR/$DR" \
    && ok "T6 manual recovery command wired in" || bad "T6 manual recovery command wired in"

echo "--- T7: syntax checks ---"
for f in device-run.sh launcher-surgery.sh test-launcher-roundtrip.sh run.sh; do
    if sh -n "$SCRIPT_DIR/$f" 2>/dev/null; then
        ok "T7 sh -n clean: $f"
    else
        bad "T7 sh -n clean: $f"
    fi
done

echo "--- T8: offline dry-run ---"
if sh "$SCRIPT_DIR/device-run.sh" --dry-run smoke >/tmp/dryrun.out 2>&1; then
    ok "T8 --dry-run smoke exits 0 with no SSH"
else
    bad "T8 --dry-run smoke exits 0 with no SSH"
fi
grep -q 'IN PLACE' /tmp/dryrun.out && ok "T8 dry-run describes in-place mechanism" \
    || bad "T8 dry-run describes in-place mechanism"
grep -q 'cp -p' /tmp/dryrun.out && bad "T8 dry-run free of cp -p" \
    || ok "T8 dry-run free of cp -p"
rm -f /tmp/dryrun.out

echo "--- T9: real EXIT-trap path from device-run.sh (stubbed SSH) ---"
# Extracts the REAL trap machinery (cleanup + restore_launcher +
# remote_restore_once + manual_restore_cmd) from device-run.sh and runs it
# in a driver shell whose miyoo_ssh/miyoo_scp are stubs executing the
# piped remote bytes against a local fixture. This proves the incident
# shape end to end: inject (or the harness) fails -> EXIT trap fires ->
# launcher restored byte-identically, script exits non-zero and loud.
T9_FUNCS=$(sed -n '/^manual_restore_cmd() {/,/^}/p; /^remote_restore_once() {/,/^}/p; /^restore_launcher() {/,/^}/p; /^cleanup() {/,/^}/p' "$SCRIPT_DIR/device-run.sh")
if [ "$(printf '%s' "$T9_FUNCS" | grep -c '() {')" -lt 4 ]; then
    bad "T9 function extraction from device-run.sh broke"
else
    ok "T9 extracted 4 trap functions from device-run.sh"
fi
T9_TMP=$(mktemp -d /tmp/uiscript-t9.XXXXXX)
T9_TMP_DRIVER="$T9_TMP/driver.sh"
# The driver inherits FIX/BAK/SUM0/SSH_FAIL_N/RESTORE_ATTEMPTS/TRAP_EXIT/
# T9_TMP/SCRIPT_DIR from the environment (exported per scenario below).
cat >"$T9_TMP_DRIVER" <<'DRIVER_EOF'
#!/bin/sh
RUNDIR="$T9_TMP/rundir"
TARGET="stub-target"
APP_DIR="$FIX"
BACKUP="$BAK"
SCRATCH="/tmp/miyoofin-ui-script-stub"
LAUNCH_PID=""
NEEDS_RESTORE=1
SCRATCH_PUSHED=0
IN_CLEANUP=0
ORIG_SUM="$SUM0"
MIYOO_SSH_PORT=2222
miyoo_ssh() {
    # $1 is the target; the rest is `sh -s -- <appdir> [...]` with the
    # remote script on stdin. Drop the target and run those exact bytes
    # locally against the fixture (APP_DIR already points at it).
    echo x >>"$T9_TMP/ssh-calls"
    if [ "$(wc -l <"$T9_TMP/ssh-calls")" -le "$SSH_FAIL_N" ]; then
        echo "stub: ssh outage (attempt $(wc -l <"$T9_TMP/ssh-calls"))" >&2
        return 1
    fi
    _tgt=$1; shift
    "$@"
}
miyoo_scp() {
    while case ${1:?} in -*) true;; *) false;; esac; do shift; done
    _src=${1:?}; _dst=${2:?}
    case $_src in "stub-target:"*) _src=${_src#"stub-target:"};; esac
    case $_dst in "stub-target:"*) _dst=${_dst#"stub-target:"};; esac
    cp "$_src" "$_dst"
}
DRIVER_EOF
printf '%s\n' "$T9_FUNCS" >>"$T9_TMP_DRIVER"
cat >>"$T9_TMP_DRIVER" <<'DRIVER_EOF'
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
exit "${TRAP_EXIT:-0}"
DRIVER_EOF
sh -n "$T9_TMP_DRIVER" 2>/dev/null && ok "T9 driver syntax clean" || bad "T9 driver syntax clean"

# Scenario A: SSH flaky twice, then recovers. Trap must retry and restore.
FIX=$(new_fixture)
export FIX
BAK=launch.sh.uiscript-bak
export BAK
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
export SUM0
miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1
mkdir -p "$T9_TMP/rundir"
cp "$REAL_LAUNCH" "$T9_TMP/rundir/launch.orig"
: >"$T9_TMP/ssh-calls"
export SSH_FAIL_N=2 RESTORE_ATTEMPTS=4 TRAP_EXIT=0
export T9_TMP SCRIPT_DIR
if sh "$T9_TMP_DRIVER" >"$T9_TMP/outA" 2>&1; then
    ok "T9-A trap exits 0 after SSH recovers"
else
    bad "T9-A trap exits 0 after SSH recovers"
fi
[ "$(wc -l <"$T9_TMP/ssh-calls")" -eq 3 ] \
    && ok "T9-A restore retried (3 SSH attempts)" || bad "T9-A restore retried ($(wc -l <"$T9_TMP/ssh-calls") SSH attempts)"
[ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ] && [ ! -e "$FIX/$BAK" ] \
    && ok "T9-A trap restored byte-identical ($SUM0), backup gone" \
    || bad "T9-A trap restored byte-identical, backup gone"
grep -q 'byte-identical to the pre-run original' "$T9_TMP/outA" \
    && ok "T9-A local byte-compare confirmed" || bad "T9-A local byte-compare confirmed"
rm -rf "$FIX"

# Scenario C (the incident shape): harness fails (exit 1) with a healthy
# link. Trap must still restore byte-identically; exit stays non-zero.
FIX=$(new_fixture)
export FIX
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
export SUM0
miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1
mkdir -p "$T9_TMP/rundir"
cp "$REAL_LAUNCH" "$T9_TMP/rundir/launch.orig"
: >"$T9_TMP/ssh-calls"
export SSH_FAIL_N=0 TRAP_EXIT=1
if sh "$T9_TMP_DRIVER" >"$T9_TMP/outC" 2>&1; then
    bad "T9-C failed harness still exits non-zero"
else
    ok "T9-C failed harness still exits non-zero"
fi
[ "$(miyoofin_launcher_sum "$FIX/launch.sh")" = "$SUM0" ] && [ ! -e "$FIX/$BAK" ] \
    && ok "T9-C trap restored byte-identical ($SUM0), backup gone" \
    || bad "T9-C trap restored byte-identical, backup gone"
rm -rf "$FIX"

# Scenario B: SSH dead. Trap must fail LOUDLY (non-zero + CRITICAL +
# manual command), never silently.
FIX=$(new_fixture)
export FIX
SUM0=$(miyoofin_launcher_sum "$FIX/launch.sh")
export SUM0
miyoofin_launcher_inject "$FIX" "$BAK" /tmp/miyoofin-ui-script 1 >/dev/null 2>&1
mkdir -p "$T9_TMP/rundir"
cp "$REAL_LAUNCH" "$T9_TMP/rundir/launch.orig"
: >"$T9_TMP/ssh-calls"
export SSH_FAIL_N=99 RESTORE_ATTEMPTS=2 TRAP_EXIT=0
if sh "$T9_TMP_DRIVER" >"$T9_TMP/outB" 2>&1; then
    bad "T9-B dead SSH exits non-zero"
else
    ok "T9-B dead SSH exits non-zero"
fi
grep -q 'CRITICAL' "$T9_TMP/outB" && ok "T9-B CRITICAL printed" || bad "T9-B CRITICAL printed"
grep -q 'ssh -p 2222 stub-target' "$T9_TMP/outB" \
    && ok "T9-B manual recovery command printed" || bad "T9-B manual recovery command printed"
[ -f "$FIX/$BAK" ] && ok "T9-B backup honestly left in place (nothing hidden)" \
    || bad "T9-B backup honestly left in place (nothing hidden)"
rm -rf "$FIX" "$T9_TMP"

echo "=== launcher round-trip: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
