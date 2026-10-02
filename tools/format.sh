#!/bin/sh
# Format first-party sources in place. ONLY entry point for bulk formatting: it
# uses the same file list as format-check.sh (never vendored/imported code), runs
# one clang-format per file with a hard per-file memory cap inside tools/bounded.sh.
set -eu
script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
cd "$script_dir/.."
cf=${CLANG_FORMAT:-clang-format}
MF_MEM=${MF_MEM:-2G} MF_TIME=${MF_TIME:-600} MF_CPU=${MF_CPU:-200} exec "$script_dir/bounded.sh" \
    sh -c '"$1"/format-sources.sh | xargs -0 -n 20 -P 2 "$2" -i --style=file' _ "$script_dir" "$cf"
