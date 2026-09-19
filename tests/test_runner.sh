#!/bin/sh
set -eu

TEST_DIR=${MIYOOFIN_TEST_DIR:-$(CDPATH= cd -- "$(dirname "$0")" && pwd)}
run_dir=$(mktemp -d "${TMPDIR:-/tmp}/miyoofin-test-run.XXXXXX")
trap 'rm -rf "$run_dir"' EXIT HUP INT TERM
TEST_BINARIES='test_catalog
test_api_session
test_api_core
test_api_events
test_ui_foundation
test_ui_models
test_cache_offline
test_artwork_episode
test_downloads
test_misc
test_playback
test_telemetry
test_telemetry_service
test_catalog_parity_query
test_catalog_parity_hierarchy
test_imagecache
test_update
test_library_coordinator
'
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

# These groups have timing-sensitive fixture/codecs; run them serially after
# the independent groups complete.
for serialized_test in test_catalog test_catalog_parity_sync test_telemetry_format test_telemetry_schema test_session
do
    if ! "$TEST_DIR/$serialized_test" >"$run_dir/$serialized_test.log" 2>&1; then
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
