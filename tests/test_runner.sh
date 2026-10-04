#!/bin/sh
set -eu

TEST_DIR=${MIYOOFIN_TEST_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/miyoofin-test-run.XXXXXX")
trap 'rm -rf "$run_dir"' EXIT HUP INT TERM
TEST_BINARIES='test_api_session
test_api_core
test_api_events
test_ui_foundation
test_ui_models
test_ui_models_monitors
test_home_artwork_controller
test_home_library_controller
test_cache_offline
test_artwork_episode
test_artwork_episode_scheduling
test_downloads
test_downloads_transfer
test_downloads_storage
test_downloads_recovery
test_misc
test_playback
test_telemetry
test_telemetry_service
test_catalog_parity_query
test_catalog_parity_hierarchy
test_imagecache
test_update
test_update_installer
test_library_coordinator
test_library_coordinator_live
test_library_coordinator_maintenance
test_library_coordinator_population
test_library_coordinator_admission
test_library_coordinator_handoff
test_library_hierarchy
test_library_query
test_worker_slot
test_player
test_music
test_music_player
test_music_downloads
test_music_screen
test_watched_sync
test_player_prefs
test_screen_lock
test_crash_report
test_http_methods
test_clock_settings
test_http_security
test_websocket
test_mpeg_ts
'
pids=
failed=
status=0

# At most MIYOOFIN_TEST_JOBS groups run at once (default: the CPU count, at most 8): launching every
# binary at once starves the timing-sensitive ones on a loaded or CPU-capped host. Each group also
# has a wall-clock limit (MIYOOFIN_TEST_TIMEOUT seconds, default 900) so a hung test fails the run
# instead of waiting for the caller's limit.
jobs=${MIYOOFIN_TEST_JOBS:-$(nproc 2>/dev/null || echo 4)}
[ "$jobs" -gt 8 ] 2>/dev/null && [ -z "${MIYOOFIN_TEST_JOBS:-}" ] && jobs=8
[ "$jobs" -ge 1 ] 2>/dev/null || jobs=4
limit=${MIYOOFIN_TEST_TIMEOUT:-900}
run_group() {
    if command -v timeout >/dev/null 2>&1; then
        timeout "$limit" "$TEST_DIR/$1" >"$run_dir/$1.log" 2>&1
    else
        "$TEST_DIR/$1" >"$run_dir/$1.log" 2>&1
    fi
}
reap_first() {
    pids=${pids# }
    entry=${pids%% *}
    pids=${pids#"$entry"}
    if ! wait "${entry%%:*}"; then
        status=1
        failed="$failed ${entry#*:}"
    fi
}
running=0
for test_binary in $TEST_BINARIES
do
    if [ "$running" -ge "$jobs" ]; then
        reap_first
        running=$((running - 1))
    fi
    run_group "$test_binary" &
    pids="$pids $!:$test_binary"
    running=$((running + 1))
done
while [ -n "${pids# }" ]
do
    reap_first
done

# These groups have timing-sensitive fixture/codecs; run them serially (once) after
# the independent groups complete.
SERIAL_BINARIES='test_catalog test_catalog_parity_sync test_telemetry_format test_telemetry_schema test_session'
for serialized_test in $SERIAL_BINARIES
do
    if ! run_group "$serialized_test"; then
        status=1
        failed="$failed $serialized_test"
    fi
done

if [ "$status" -ne 0 ]; then
    for test_binary in $TEST_BINARIES $SERIAL_BINARIES
    do
        [ -f "$run_dir/$test_binary.log" ] && cat "$run_dir/$test_binary.log"
    done
    printf '%s\n' "[test] FAILED groups:$failed" >&2
    exit "$status"
fi

for test_binary in $TEST_BINARIES $SERIAL_BINARIES
do
    cat "$run_dir/$test_binary.log"
done

printf '%s\n' '[test] all split groups passed'
