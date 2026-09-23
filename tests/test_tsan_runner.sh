#!/bin/sh
set -eu

# Focused ThreadSanitizer runner for the host concurrency suites.  The Makefile
# target copies this script into the isolated output/tsan/test tree and invokes
# it with MIYOOFIN_TSAN_GROUPS set to the same list it built dependencies for,
# so the binary list has a single source of truth.  Running the script directly
# falls back to that same default list.
#
# Design constraints:
#   * serial: TSan reports are only meaningful one process at a time, and the
#     focused suites already use internal workers;
#   * fail on any non-zero exit (TSan exits 66 when it detects a race) and on
#     any ThreadSanitizer marker in a log, even if a binary somehow exits 0;
#   * preserve per-binary logs so a report survives for triage; never delete
#     them on failure.
#   * no suppressions: first-party races must be fixed, not silenced.

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
log_dir="$script_dir/logs"
mkdir -p "$log_dir"

groups=${MIYOOFIN_TSAN_GROUPS:-library_coordinator library_hierarchy catalog home_library_controller home_artwork_controller downloads}

# Fail fast on the first report; still return a non-zero process status.
# An explicit caller TSAN_OPTIONS override is respected for triage runs.
TSAN_OPTIONS=${TSAN_OPTIONS:-halt_on_error=1:abort_on_error=0}
export TSAN_OPTIONS

status=0
for group in $groups
do
    binary="$script_dir/test_$group"
    log="$log_dir/test_$group.log"
    if [ ! -x "$binary" ]; then
        echo "tsan: missing test binary $binary" >&2
        status=1
        continue
    fi
    printf '=== tsan test_%s ===\n' "$group"
    if "$binary" >"$log" 2>&1; then
        printf '  [ok] test_%s\n' "$group"
    else
        rc=$?
        status=1
        printf '  [FAIL] test_%s (exit %s, log: %s)\n' "$group" "$rc" "$log"
    fi
    if grep -q 'ThreadSanitizer' "$log"; then
        status=1
        printf '  [REPORT] ThreadSanitizer output in %s\n' "$log"
    fi
done

if [ "$status" -ne 0 ]; then
    echo
    echo "=== ThreadSanitizer logs ==="
    for group in $groups
    do
        log="$log_dir/test_$group.log"
        [ -f "$log" ] || continue
        if grep -q 'ThreadSanitizer' "$log"; then
            echo "--- $log ---"
            cat "$log"
        fi
    done
    echo "tsan: FAILED"
    exit 1
fi

printf '%s\n' 'tsan: all focused suites passed'
