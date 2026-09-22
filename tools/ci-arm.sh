#!/bin/sh
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/.." && pwd)
cd "$repo_root"

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker is required for the ARM cross-build." >&2
    exit 1
fi
if ! docker image inspect miyoofin-toolchain >/dev/null 2>&1; then
    echo "ERROR: required Docker image miyoofin-toolchain is missing." >&2
    echo "Build it with 'docker build -f Dockerfile.onionos -t miyoofin-toolchain .' or pull the CI image." >&2
    exit 1
fi

mkdir -p output/build-arm
docker run --rm --user "$(id -u):$(id -g)" -v "$repo_root:/build" miyoofin-toolchain \
    make -f Makefile.cross PERF_TELEMETRY="${PERF_TELEMETRY:-1}" RELEASE="${RELEASE:-1}" all bridge reporter

make verify-arm
