#!/bin/sh
set -eu

TEST_DIR=${MIYOOFIN_TEST_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/miyoofin-test-run.XXXXXX")
trap 'rm -rf "$run_dir"' EXIT HUP INT TERM
TEST_BINARIES='test_catalog
test_api_session
test_ui_foundation
test_cache_offline
test_artwork_episode
test_downloads
test_misc
test_playback
test_telemetry
test_telemetry_format
test_telemetry_service
test_telemetry_schema'
pids=

for test_binary in $TEST_BINARIES
do
    "$TEST_DIR/$test_binary" >"$run_dir/$test_binary.log" 2>&1 &
    pids="$pids $!"
done

status=0
for pid in $pids
do
    if ! wait "$pid"; then
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    for test_binary in $TEST_BINARIES
    do
        cat "$run_dir/$test_binary.log"
    done
    exit "$status"
fi

for test_binary in $TEST_BINARIES
do
    cat "$run_dir/$test_binary.log"
done

printf '%s\n' '[test] all split groups passed'
