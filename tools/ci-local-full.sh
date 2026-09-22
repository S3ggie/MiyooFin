#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

sh "$script_dir/ci-local.sh"

echo "=== ARM cross-build and verification ==="
make onionos
make verify-arm
