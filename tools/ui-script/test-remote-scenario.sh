#!/bin/sh
# Focused syntax/static test for the observer-backed remote scenario slice.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
TMP=$(mktemp -d /tmp/miyoofin-remote-scenario-test.XXXXXX)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

fail() { echo "remote-scenario-test: FAIL: $*" >&2; exit 1; }
ok() { echo "remote-scenario-test: OK: $*"; }

for file in remote-observer.sh remote-scenario.sh device-run.sh; do
    sh -n "$SCRIPT_DIR/$file" || fail "$file has invalid shell syntax"
done
ok "observer, wrapper, and device runner shell syntax"

grep -q 'remote_observer_snapshot()' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'snapshot helper missing'
grep -q 'remote_observer_postcondition()' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'postcondition helper missing'
grep -q '/proc/\[0-9\].*/comm' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer does not sample process state'
grep -q 'VmRSS:' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer does not sample memory'
grep -q 'Threads:' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer does not sample thread counts'
grep -q 'url-redacted' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer log redaction missing'
grep -q 'remote_observer_wait_postcondition' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'wrapper does not enforce the final postcondition'
grep -q 'trap cleanup EXIT' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'wrapper has no cleanup trap'
grep -q 'download-home-absent) RUNNER_NAME=dl-start' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'download-home-absent does not reuse the existing dl-start flow'
grep -q 'stability|home-reentry) RUNNER_NAME=home-reentry' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'stability does not reuse the existing Home event flow'
grep -q 'download_state' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer does not sample download state'
grep -q 'remote_observer_no_duplicate_processes' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'stability does not check duplicate MainUI/MiyooFin processes'
grep -q 'PASS scenario=\$NAME semantics=stability-alias' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'home-reentry has no explicit stability-alias result semantics'
grep -q 'sleep_until_next_sample' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'sampling interval is not bounded while waiting for the runner'
grep -q 'unsafe characters' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'remote configurable paths are not validated'
grep -q 'UNRUN' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'unsupported scenarios are not marked UNRUN'
ok "observer and cleanup/static invariants"

SCRIPT="$SCRIPT_DIR/scripts/home-reentry-device.txt"
[ -f "$SCRIPT" ] || fail 'home-reentry scenario missing'
grep -q '^WAIT_LOG \[HomeScreen\] Library loaded' "$SCRIPT" \
    || fail 'scenario has no bounded Home postcondition wait'
[ "$(grep -c '^KEY NextTab' "$SCRIPT")" -eq 3 ] \
    || fail 'scenario does not leave Home three times'
[ "$(grep -c '^KEY PrevTab' "$SCRIPT")" -eq 3 ] \
    || fail 'scenario does not re-enter Home three times'
grep -q '^SCREENSHOT home-reentry$' "$SCRIPT" \
    || fail 'scenario has no final screenshot'
tail -n 1 "$SCRIPT" | grep -qx 'QUIT' \
    || fail 'scenario does not end with QUIT'
ok "repeated Home leave/re-entry script"

sh "$SCRIPT_DIR/remote-scenario.sh" physical-playback >"$TMP/playback.out" \
    || fail 'physical playback UNRUN should be non-fatal'
grep -q '^UNRUN scenario=physical-playback' "$TMP/playback.out" \
    || fail 'physical playback was not marked UNRUN'
sh "$SCRIPT_DIR/remote-scenario.sh" suspend >"$TMP/suspend.out" \
    || fail 'suspend UNRUN should be non-fatal'
grep -q '^UNRUN scenario=suspend' "$TMP/suspend.out" \
    || fail 'suspend was not marked UNRUN'
ok "physical playback/suspend are explicitly UNRUN"

MIYOO_SSH_TARGET=test-target sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run download-home-absent >"$TMP/download-dry-run.out" \
    || fail 'download-home-absent dry-run failed'
grep -q 'scripts/dl-start-device.txt' "$TMP/download-dry-run.out" \
    || fail 'download-home-absent dry-run did not use dl-start'
MIYOO_SSH_TARGET=test-target sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run stability >"$TMP/stability-dry-run.out" \
    || fail 'stability dry-run failed'
grep -q 'scripts/home-reentry-device.txt' "$TMP/stability-dry-run.out" \
    || fail 'stability dry-run did not use home-reentry'
ok "supported scenario dry-runs preserve existing UI flows"

for interval_case in '0:1' '-5:3' '999999999999999999999999:30'; do
    interval=${interval_case%:*}
    expected=${interval_case#*:}
    MIYOO_SSH_TARGET=test-target MIYOOFIN_REMOTE_SAMPLE_INTERVAL_S="$interval" \
        sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run stability >"$TMP/interval-$expected.out" \
        || fail "sample interval $interval dry-run failed"
    grep -q "sampling interval=${expected}s" "$TMP/interval-$expected.out" \
        || fail "sample interval $interval was not normalized to $expected"
done
ok "sampling intervals are validated and bounded"

if MIYOO_SSH_TARGET=test-target MIYOOFIN_REMOTE_LOG='/tmp/app log' \
    sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run stability >"$TMP/unsafe-path.out" 2>&1; then
    fail 'unsafe remote log path was accepted'
fi
grep -q 'MIYOOFIN_REMOTE_LOG contains unsafe characters' "$TMP/unsafe-path.out" \
    || fail 'unsafe remote log path did not produce a clear rejection'
if MIYOOFIN_DEVICE_APP_DIR='/mnt/SDCARD/App/MiyooFin;bad' \
    sh "$SCRIPT_DIR/device-run.sh" --dry-run smoke >"$TMP/unsafe-app-dir.out" 2>&1; then
    fail 'unsafe device app path was accepted'
fi
grep -q 'MIYOOFIN_DEVICE_APP_DIR contains unsafe characters' "$TMP/unsafe-app-dir.out" \
    || fail 'unsafe device app path did not produce a clear rejection'
ok "remote configurable paths reject unsafe characters"

# Exercise the sourceable observer with a tiny fake SSH endpoint so this
# focused test does not need hardware or a configured key.
miyoo_ssh() {
    _target=$1
    shift
    if [ "${FAKE_MODE:-}" = snapshot ]; then
        "$@" >"$TMP/fake-remote.out"
        cat <<'EOF'
sample_epoch=1
process name=MainUI count=1 pids=10 rss_kb=20 threads=2
process name=miyoofin count=1 pids=11 rss_kb=30 threads=4
download_state root=/app/downloads manifests=1 media_files=2 partial_files=1 complete_items=0 active_items=1 downloaded_bytes=30 expected_bytes=100
EOF
        cat "$TMP/fake-remote.out"
    else
        printf 'POSTCONDITION OK clean\n'
    fi
    return 0
}
# shellcheck disable=SC1091
. "$SCRIPT_DIR/remote-observer.sh"
cat >"$TMP/app.log" <<'EOF'
request https://example.invalid/items?api_key=api-secret
X-Emby-Token: emby-token-secret
X-Emby-Authorization: Token="emby-authorization-secret"
EOF
FAKE_MODE=snapshot remote_observer_snapshot fake "$TMP/snapshot" "$TMP/app.log" 3
grep -q 'process name=miyoofin count=1.*rss_kb=30.*threads=4' "$TMP/snapshot" \
    || fail 'fake snapshot did not preserve process metrics'
grep -q '<url-redacted>' "$TMP/snapshot" \
    || fail 'snapshot test did not retain redaction marker'
grep -q '^download_state .*downloaded_bytes=30 .*expected_bytes=100' "$TMP/snapshot" \
    || fail 'snapshot test did not retain download metrics'
if grep -qE 'api-secret|emby-token-secret|emby-authorization-secret' "$TMP/snapshot"; then
    fail 'snapshot observations retained an unredacted token'
fi
grep -q 'X-Emby-Token: <redacted>' "$TMP/snapshot" \
    || fail 'X-Emby-Token header was not redacted'
grep -q 'X-Emby-Authorization: Token=<redacted>' "$TMP/snapshot" \
    || fail 'X-Emby-Authorization Token form was not redacted'
remote_observer_no_duplicate_processes "$TMP/snapshot" \
    || fail 'duplicate-process helper rejected a unique process snapshot'
FAKE_MODE=postcondition remote_observer_postcondition fake clean /app launch.sh.uiscript-bak
ok "sourceable observer smoke path"

echo 'remote-scenario-test: PASS'
