#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

if ! command -v clang-format-18 >/dev/null 2>&1; then
    echo "ERROR: clang-format-18 is required for ci-local; install the CI formatter." >&2
    exit 1
fi
if ! command -v xvfb-run >/dev/null 2>&1; then
    echo "ERROR: xvfb-run is required for ci-local host tests." >&2
    exit 1
fi

echo "=== First-party formatting (clang-format-18) ==="
CLANG_FORMAT=clang-format-18 make format-check

echo "=== Host build ==="
make -j2

echo "=== Tests and refactor/boundary checks ==="
# refactor-check owns the normal make test invocation and its diff check.
xvfb-run -a make refactor-check

echo "=== Final whitespace check ==="
git diff --check
