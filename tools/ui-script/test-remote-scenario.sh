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
grep -q 'offline-clean' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'offline playback has no strict MainUI/restoration postcondition'
OFFLINE_CLEAN_BLOCK=$(awk '/offline-clean\)/,/;;/' "$SCRIPT_DIR/remote-observer.sh")
for playback_process in ffplay bridge reporter; do
    printf '%s\n' "$OFFLINE_CLEAN_BLOCK" \
        | grep -Fq "[ \"\$$playback_process\" -eq 0 ]" \
        || fail "offline-clean does not require zero $playback_process processes"
done
grep -q 'describe_processes ffplay' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'offline playback does not sample ffplay'
grep -q 'framebuffer_state' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'offline playback does not sample framebuffer evidence'
grep -q 'scope=audio-video' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'offline playback does not disclose audio/video measurement limitation'
grep -q 'onion-remote-exit.sh' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'offline playback does not use the supported exit control'
grep -q 'PLAYBACK_LOG_OFFSET' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'offline playback has no per-run playback log boundary'
grep -q 'tail -c' "$SCRIPT_DIR/remote-observer.sh" \
    || fail 'observer does not apply the playback log boundary'
grep -q 'PASS scenario=\$NAME semantics=stability-alias' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'home-reentry has no explicit stability-alias result semantics'
grep -q 'sleep_until_next_sample' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'sampling interval is not bounded while waiting for the runner'
grep -q 'unsafe characters' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'remote configurable paths are not validated'
grep -q 'UNRUN' "$SCRIPT_DIR/remote-scenario.sh" \
    || fail 'unsupported scenarios are not marked UNRUN'
ok "observer and cleanup/static invariants"

# The screenshot source is the remote scratch directory, not the host output
# directory.  Exercise the exact device-run cleanup command with a local fake
# SSH endpoint: stale remote content is removed before launch, while a fresh
# post-clear artifact remains available to be pulled and asserted.
awk '/^clear_offline_playback_screenshot\(\)/,/^}/' "$SCRIPT_DIR/device-run.sh" \
    >"$TMP/clear-offline-playback-screenshot.sh"
# shellcheck disable=SC1091
. "$TMP/clear-offline-playback-screenshot.sh"
REMOTE_SHOT="$TMP/remote-scratch/shots/offline-playback-downloads.bmp"
mkdir -p "$(dirname "$REMOTE_SHOT")"
NAME=offline-playback
TARGET=test-target
SCRATCH="$TMP/remote-scratch"
miyoo_ssh() {
    _target=$1
    _command=$2
    [ "$_target" = test-target ] || fail "screenshot cleanup used the wrong target"
    sh -c "$_command"
}
clear_offline_playback_screenshot \
    || fail 'stale remote screenshot cleanup failed'
[ ! -e "$REMOTE_SHOT" ] \
    || fail 'stale remote screenshot survived pre-launch cleanup'
ok "stale remote screenshot is removed before offline playback launch"
printf 'fresh remote framebuffer\n' >"$REMOTE_SHOT"
[ -s "$REMOTE_SHOT" ] \
    || fail 'fresh remote screenshot was not available after pre-launch cleanup'
ok "fresh post-clear remote screenshot remains available as run evidence"

# Pulling result.txt must distinguish an intentionally absent remote file from
# a failed SSH/SCP operation.  Exercise the exact production helper with a
# fake SSH endpoint: only offline-playback's bounded exited verdict may create
# an empty local result, while missing-file, transport, and SCP failures fail.
awk '/^result_absence_is_expected_bounded_exit\(\)/,/^}/; /^pull_result_txt\(\)/,/^}/' \
    "$SCRIPT_DIR/device-run.sh" >"$TMP/result-pull-functions.sh"
(
    fail() { echo "result-pull-test: $*" >&2; exit 1; }
    FAKE_RESULT_STATE=absent
    FAKE_SCP_RC=0
    FAKE_SCP_CALLS=0
    miyoo_ssh() {
        [ "$1" = test-target ] || return 255
        case "$FAKE_RESULT_STATE" in
            transport) return 255 ;;
            absent|present) printf '%s' "$FAKE_RESULT_STATE"; return 0 ;;
            *) return 255 ;;
        esac
    }
    miyoo_scp() {
        FAKE_SCP_CALLS=$((FAKE_SCP_CALLS + 1))
        return "$FAKE_SCP_RC"
    }
    # shellcheck disable=SC1091
    . "$TMP/result-pull-functions.sh"
    TARGET=test-target
    SCRATCH="$TMP/result-remote"
    OUT="$TMP/result-output"
    mkdir -p "$OUT"

    NAME=offline-playback
    VERDICT=exited
    pull_result_txt || fail 'expected bounded-exit absence was rejected'
    [ -f "$OUT/result.txt" ] || fail 'bounded-exit absence did not create result.txt'
    [ ! -s "$OUT/result.txt" ] || fail 'bounded-exit absence created non-empty result.txt'
    [ "$FAKE_SCP_CALLS" -eq 0 ] || fail 'absent result unexpectedly invoked SCP'

    VERDICT=timeout
    if ( pull_result_txt 2>"$TMP/absent-unexpected.err" ); then
        fail 'absent result outside bounded-exit path was accepted'
    fi
    grep -q 'absent outside the expected bounded-exit path' "$TMP/absent-unexpected.err" \
        || fail 'unexpected absent result did not report the bounded-exit guard'

    VERDICT=exited
    FAKE_RESULT_STATE=present
    FAKE_SCP_RC=1
    if ( pull_result_txt 2>"$TMP/scp-failure.err" ); then
        fail 'SCP failure was suppressed'
    fi
    grep -q 'pull result.txt failed' "$TMP/scp-failure.err" \
        || fail 'SCP failure did not report a pull error'

    FAKE_RESULT_STATE=transport
    FAKE_SCP_RC=0
    if ( pull_result_txt 2>"$TMP/ssh-failure.err" ); then
        fail 'SSH transport failure was suppressed'
    fi
    grep -q 'SSH/transport failure' "$TMP/ssh-failure.err" \
        || fail 'SSH transport failure did not report an inspection error'
)
ok "result.txt absence is distinguished from fake SSH/SCP failures"

# Player evidence must be current-run evidence.  A baseline PID and screenshot
# artifact that are unchanged after the run must not satisfy the verdict.
FAKE_EVIDENCE_ROOT="$TMP/evidence-root"
mkdir -p "$FAKE_EVIDENCE_ROOT/output/ui-script/device-offline-playback/shots"
FAKE_PLAYBACK_SHOT="$FAKE_EVIDENCE_ROOT/output/ui-script/device-offline-playback/shots/offline-playback-downloads.bmp"
printf 'stale framebuffer\n' >"$FAKE_PLAYBACK_SHOT"
cat >"$TMP/evidence-observations.log" <<EOF
download_state complete_items=1
Starting external playback handoff
process name=ffplay count=1 pids=41 rss_kb=40 threads=5
process name=miyoofin-https-bridge count=1 pids=42 rss_kb=10 threads=1
process name=miyoofin-playback-reporter count=1 pids=43 rss_kb=8 threads=1
playback_log_tail_begin path=/app/playback-launch.log lines=40
playback_log_tail_end
EOF
OBSERVATIONS="$TMP/evidence-observations.log"
PLAYBACK_SHOT="$FAKE_PLAYBACK_SHOT"
BASELINE_CAPTURED=1
BASELINE_FFPLAY_PIDS=41
BASELINE_BRIDGE_PIDS=42
BASELINE_REPORTER_PIDS=43
PLAYBACK_SHOT_EXISTED=1
PLAYBACK_SHOT_BASELINE_MTIME=$(stat -c '%Y:%y' "$PLAYBACK_SHOT")
PLAYBACK_SHOT_BASELINE_HASH=$(sha256sum "$PLAYBACK_SHOT" | awk '{ print $1 }')
completed_download_count() {
    awk '
        $1 == "download_state" {
            for (i = 1; i <= NF; i++) {
                split($i, kv, "=")
                if (kv[1] == "complete_items") value = kv[2]
            }
        }
        END { print value + 0 }
    ' "$OBSERVATIONS"
}
awk '/^playback_pid_is_new\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >"$TMP/offline-playback-evidence.sh"
awk '/^offline_playback_evidence\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >>"$TMP/offline-playback-evidence.sh"
# shellcheck disable=SC1091
. "$TMP/offline-playback-evidence.sh"
set -- $(offline_playback_evidence)
[ "$3" -eq 0 ] || fail 'stale playback evidence produced local-source evidence'
[ "$4" -eq 0 ] || fail 'stale player PIDs produced current-run player evidence'
[ "$5" -eq 0 ] || fail 'stale framebuffer artifact produced current-run evidence'
ok "stale player PIDs and framebuffer artifact cannot produce PASS evidence"

# A newly observed child PID and a changed framebuffer artifact are sufficient
# current-run evidence even when the launch log is empty.
printf 'fresh framebuffer\n' >>"$PLAYBACK_SHOT"
sed 's/pids=41/pids=99/; s/pids=42/pids=100/; s/pids=43/pids=101/' \
    "$TMP/evidence-observations.log" >"$TMP/fresh-evidence-observations.log"
OBSERVATIONS="$TMP/fresh-evidence-observations.log"
set -- $(offline_playback_evidence)
[ "$4" -eq 1 ] || fail 'new player PID did not produce current-run evidence'
[ "$5" -eq 1 ] || fail 'changed framebuffer artifact did not produce current-run evidence'
ok "new player PID and changed framebuffer artifact produce current-run evidence"

# Inject a failed supported-exit helper after all other offline cleanup has
# succeeded.  This is the old reviewer-blocking path: cleanup must not turn
# that failure into a PASS merely because offline-clean later succeeds.
cat >"$TMP/pass-shaped-observations.log" <<'EOF'
download_state complete_items=1
Starting external playback handoff
process name=ffplay count=1
playback_log_tail_begin path=/app/playback-launch.log lines=40
External playback request source_mode=local
ffplay_spawned pid=41
playback_log_tail_end
EOF
OBSERVATIONS="$TMP/pass-shaped-observations.log"
NAME=offline-playback
DRY_RUN=0
SAMPLE_COUNT=1
OBSERVER_FAILED=0
POSTCONDITION_OK=0
SUPPORTED_EXIT_REQUESTED=0
SUPPORTED_EXIT_SUCCEEDED=0
RUNDIR="$TMP/exit-failure-rundir"
mkdir -p "$RUNDIR" "$TMP/failing-root/tools/miyoo"
cat >"$TMP/failing-root/tools/miyoo/onion-remote-exit.sh" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$TMP/failing-root/tools/miyoo/onion-remote-exit.sh"
PLAYBACK_SHOT="$FAKE_PLAYBACK_SHOT"
PLAYBACK_SHOT_EXISTED=0
PLAYBACK_SHOT_BASELINE_MTIME=
PLAYBACK_SHOT_BASELINE_HASH=
BASELINE_CAPTURED=1
BASELINE_FFPLAY_PIDS=41
BASELINE_BRIDGE_PIDS=42
BASELINE_REPORTER_PIDS=43
awk '/^offline_playback_cleanup_override_allowed\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >"$TMP/offline-playback-cleanup-override.sh"
awk '/^runner_failure_is_expected_bounded_exit\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >"$TMP/offline-playback-runner-failure.sh"
cat "$TMP/offline-playback-runner-failure.sh" \
    "$TMP/offline-playback-cleanup-override.sh" >"$TMP/offline-playback-cleanup-functions.sh"
awk '/^request_supported_exit\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >>"$TMP/offline-playback-cleanup-functions.sh"
awk '/^report_scenario\(\)/,/^}/' "$SCRIPT_DIR/remote-scenario.sh" \
    >>"$TMP/offline-playback-cleanup-functions.sh"
# shellcheck disable=SC1091
. "$TMP/offline-playback-cleanup-functions.sh"
ROOT="$TMP/failing-root"
RUNNER_LOG="$TMP/runner.log"
POSTCONDITION_OK=1
SUPPORTED_EXIT_SUCCEEDED=1
cat >"$RUNNER_LOG" <<'EOF'
device-run(offline-playback): no clean script verdict (app exited; see output)
EOF
if ! offline_playback_cleanup_override_allowed 1; then
    fail 'expected bounded exit was not eligible for cleanup override'
fi
for runner_failure in assertion pull; do
    case "$runner_failure" in
        assertion)
            printf '%s\n' \
                'device-run(offline-playback): script driver reported failure' \
                >"$RUNNER_LOG"
            ;;
        pull)
            printf '%s\n' \
                'device-run(offline-playback): pull shots failed' \
                >"$RUNNER_LOG"
            ;;
    esac
    if offline_playback_cleanup_override_allowed 1; then
        fail "$runner_failure failure was allowed to override cleanup"
    fi
done
ok "only the expected bounded exit can override a runner failure"

SUPPORTED_EXIT_SUCCEEDED=0
if request_supported_exit >"$TMP/exit-failure.out" 2>&1; then
    fail 'injected supported-exit helper failure unexpectedly succeeded'
fi
[ "$SUPPORTED_EXIT_REQUESTED" = 1 ] || fail 'failed supported exit was not recorded as requested'
[ "$SUPPORTED_EXIT_SUCCEEDED" = 0 ] || fail 'failed supported exit was recorded as successful'
POSTCONDITION_OK=1
if offline_playback_cleanup_override_allowed; then
    fail 'failed supported exit was allowed to override successful offline-clean'
fi
if report_scenario 0 | grep -q '^PASS scenario=offline-playback'; then
    fail 'failed supported exit produced PASS-shaped offline-playback result'
fi
ok "failed supported exit cannot become PASS after offline-clean"

# Cleanup must not override a runner failure when the player and screenshot
# evidence are present but the playback handoff or local-source proof is not.
SUPPORTED_EXIT_SUCCEEDED=1
cat >"$RUNNER_LOG" <<'EOF'
device-run(offline-playback): no clean script verdict (app exited; see output)
EOF
for missing_evidence in handoff local-source; do
    case "$missing_evidence" in
        handoff)
            cat >"$TMP/missing-handoff-observations.log" <<'EOF'
download_state complete_items=1
process name=ffplay count=1
playback_log_tail_begin path=/app/playback-launch.log lines=40
External playback request source_mode=local
ffplay_spawned pid=41
playback_log_tail_end
EOF
            ;;
        local-source)
            cat >"$TMP/missing-local-source-observations.log" <<'EOF'
download_state complete_items=1
Starting external playback handoff
process name=ffplay count=1
playback_log_tail_begin path=/app/playback-launch.log lines=40
External playback request source_mode=remote
ffplay_spawned pid=41
playback_log_tail_end
EOF
            ;;
    esac
    OBSERVATIONS="$TMP/missing-$missing_evidence-observations.log"
    if offline_playback_cleanup_override_allowed; then
        fail "missing $missing_evidence evidence was allowed to override runner failure"
    fi
    if report_scenario 1 | grep -q '^PASS scenario=offline-playback'; then
        fail "missing $missing_evidence evidence produced PASS-shaped result"
    fi
done
ok "missing playback handoff/local evidence preserves runner failure"

SCRIPT="$SCRIPT_DIR/scripts/offline-playback-device.txt"
[ -f "$SCRIPT" ] || fail 'offline-playback scenario missing'
grep -q '^KEY NextTab 3$' "$SCRIPT" \
    || fail 'offline-playback does not navigate with existing tab SDL events'
[ "$(grep -c '^KEY NextTab 3$' "$SCRIPT")" -eq 3 ] \
    || fail 'offline-playback does not navigate to Downloads'
grep -q '^SCREENSHOT offline-playback-downloads$' "$SCRIPT" \
    || fail 'offline-playback has no Downloads framebuffer screenshot'
grep -q '^KEY Confirm 3 ' "$SCRIPT" \
    || fail 'offline-playback does not select the completed row'
grep -q '^WAIT_LOG \[App\] Starting external playback handoff' "$SCRIPT" \
    || fail 'offline-playback has no playback handoff marker'
grep -q '^SETTLE 600000$' "$SCRIPT" \
    || fail 'offline-playback does not leave a bounded-wrapper-controlled playback interval'
ok "offline playback UI event script"

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
MIYOO_SSH_TARGET=test-target MIYOOFIN_OFFLINE_PLAYBACK_INTERVAL_S=7 \
    sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run offline-playback >"$TMP/offline-playback-dry-run.out" \
    || fail 'offline-playback dry-run failed'
grep -q 'scripts/offline-playback-device.txt' "$TMP/offline-playback-dry-run.out" \
    || fail 'offline-playback dry-run did not use its explicit UI script'
grep -q 'offline playback interval=7s' "$TMP/offline-playback-dry-run.out" \
    || fail 'offline-playback dry-run omitted its bounded interval'
ok "offline playback dry-run preserves explicit controls"

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

for playback_interval_case in '0:1' '-5:30' '999999999999999999999999:300'; do
    interval=${playback_interval_case%:*}
    expected=${playback_interval_case#*:}
    MIYOO_SSH_TARGET=test-target MIYOOFIN_OFFLINE_PLAYBACK_INTERVAL_S="$interval" \
        sh "$SCRIPT_DIR/remote-scenario.sh" --dry-run offline-playback >"$TMP/playback-interval-$expected.out" \
        || fail "offline playback interval $interval dry-run failed"
    grep -q "offline playback interval=${expected}s" "$TMP/playback-interval-$expected.out" \
        || fail "offline playback interval $interval was not normalized to $expected"
done
ok "offline playback intervals are validated and bounded"

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
process name=ffplay count=1 pids=12 rss_kb=40 threads=5
process name=miyoofin-https-bridge count=1 pids=13 rss_kb=10 threads=1
process name=miyoofin-playback-reporter count=1 pids=14 rss_kb=8 threads=1
framebuffer path=/dev/fb0 readable=1
download_state root=/app/downloads manifests=1 media_files=2 partial_files=1 complete_items=0 active_items=1 downloaded_bytes=30 expected_bytes=100
playback_log_tail_begin path=/app/playback-launch.log lines=3
External playback request source_mode=local
ffplay_spawned pid=12
playback_log_tail_end
EOF
        cat "$TMP/fake-remote.out"
    elif [ "${FAKE_MODE:-}" = real-snapshot ]; then
        # Run the exact remote shell payload locally.  In particular, this
        # exercises the production redacted_tail/sanitizer rather than
        # asserting against output that was already redacted by the fixture.
        "$@"
    elif [ "${FAKE_MODE:-}" = postcondition ]; then
        # Execute the actual remote postcondition script against a fake proc
        # tree.  This keeps the cleanup test independent of device hardware
        # while proving that each playback child is part of the remote check.
        if sed "s#/proc/#$FAKE_PROC_ROOT/#g" | "$@" >"$TMP/postcondition.out"; then
            cat "$TMP/postcondition.out"
            return 0
        fi
        return 1
    else
        printf 'POSTCONDITION OK clean\n'
    fi
    return 0
}

FAKE_PROC_ROOT="$TMP/fake-proc"
reset_fake_postcondition() {
    rm -rf "$FAKE_PROC_ROOT" "$TMP/fake-app"
    mkdir -p "$FAKE_PROC_ROOT/10" "$TMP/fake-app"
    printf 'MainUI\n' >"$FAKE_PROC_ROOT/10/comm"
    printf '#!/bin/sh\n' >"$TMP/fake-app/launch.sh"
    chmod +x "$TMP/fake-app/launch.sh"
}
fake_process() {
    _pid=$1
    _name=$2
    mkdir -p "$FAKE_PROC_ROOT/$_pid"
    printf '%s\n' "$_name" >"$FAKE_PROC_ROOT/$_pid/comm"
}
# shellcheck disable=SC1091
. "$SCRIPT_DIR/remote-observer.sh"
cat >"$TMP/app.log" <<'EOF'
request https://example.invalid/items?api_key=api-secret
X-Emby-Token: emby-token-secret
X-Emby-Authorization: Token="emby-authorization-secret"
Authorization: Bearer authorization-secret
redirect=https://example.invalid/stream?access_token=access-token-secret
EOF
FAKE_MODE=snapshot remote_observer_snapshot fake "$TMP/snapshot" "$TMP/app.log" 20 /app/downloads /app/playback-launch.log
grep -q 'process name=miyoofin count=1.*rss_kb=30.*threads=4' "$TMP/snapshot" \
    || fail 'fake snapshot did not preserve process metrics'
grep -q '<url-redacted>' "$TMP/snapshot" \
    || fail 'snapshot test did not retain redaction marker'
grep -q '^download_state .*downloaded_bytes=30 .*expected_bytes=100' "$TMP/snapshot" \
    || fail 'snapshot test did not retain download metrics'
grep -q 'process name=ffplay count=1' "$TMP/snapshot" \
    || fail 'fake snapshot did not preserve player process metrics'
grep -q 'framebuffer path=/dev/fb0 readable=1' "$TMP/snapshot" \
    || fail 'fake snapshot did not preserve framebuffer evidence'
grep -q 'source_mode=local' "$TMP/snapshot" \
    || fail 'fake snapshot did not preserve local playback evidence'
if grep -qE 'api-secret|emby-token-secret|emby-authorization-secret' "$TMP/snapshot"; then
    fail 'snapshot observations retained an unredacted token'
fi
grep -q 'X-Emby-Token: <redacted>' "$TMP/snapshot" \
    || fail 'X-Emby-Token header was not redacted'
grep -q 'X-Emby-Authorization: Token=<redacted>' "$TMP/snapshot" \
    || fail 'X-Emby-Authorization Token form was not redacted'
remote_observer_no_duplicate_processes "$TMP/snapshot" \
    || fail 'duplicate-process helper rejected a unique process snapshot'
ok "sourceable observer smoke path"

FAKE_PLAYBACK_LOG="$TMP/playback-launch.log"
cat >"$FAKE_PLAYBACK_LOG" <<'EOF'
External playback request source_mode=local
request https://example.invalid/items?ApiKey=full-url-secret
X-Emby-Token: playback-token-secret
EOF
FAKE_MODE=real-snapshot remote_observer_snapshot fake "$TMP/redaction-snapshot" \
    "$TMP/app.log" 20 '' "$FAKE_PLAYBACK_LOG"
if grep -qE 'api-secret|emby-token-secret|emby-authorization-secret|authorization-secret|access-token-secret|full-url-secret|playback-token-secret' \
    "$TMP/redaction-snapshot"; then
    fail 'real redacted_tail payload retained a token or full URL secret'
fi
grep -q '<url-redacted>' "$TMP/redaction-snapshot" \
    || fail 'real redacted_tail payload did not redact a full URL'
grep -q 'X-Emby-Token: <redacted>' "$TMP/redaction-snapshot" \
    || fail 'real redacted_tail payload did not redact a token header'
ok "real redacted_tail sanitizer removes token/full-URL secrets"

for playback_process in ffplay miyoofin-https-bridge miyoofin-playback-reporter; do
    reset_fake_postcondition
    fake_process 12 "$playback_process"
    if FAKE_MODE=postcondition remote_observer_postcondition \
        fake offline-clean "$TMP/fake-app" launch.sh.uiscript-bak; then
        fail "offline-clean accepted nonzero $playback_process process count"
    fi
done
reset_fake_postcondition
FAKE_MODE=postcondition remote_observer_postcondition \
    fake offline-clean "$TMP/fake-app" launch.sh.uiscript-bak \
    || fail 'offline-clean rejected zero playback child process counts'
ok "offline-clean rejects playback children and accepts zero counts"

echo 'remote-scenario-test: PASS'
