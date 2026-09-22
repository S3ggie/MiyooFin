#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

sh "$script_dir/ci-local-host.sh"

if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "ERROR: xvfb-run is required for ci-local sanitizer and UI tests." >&2
    exit 1
fi

echo "=== ASan+UBSan tests ==="
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:abort_on_error=0:strict_string_checks=1}" \
UBSAN_OPTIONS="${UBSAN_OPTIONS:-print_stacktrace=1:halt_on_error=1}" \
xvfb-run -a make test-sanitize -j2

echo "=== Scripted UI tests ==="
xvfb-run -a make ui-script-test
