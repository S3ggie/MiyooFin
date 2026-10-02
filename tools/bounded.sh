#!/bin/sh
# Run a heavy command (formatter, build, CI) inside a user systemd cgroup scope
# so its whole process tree has a hard memory/swap/task/CPU/time budget. A runaway
# job is killed by the kernel inside the scope instead of pressuring the host
# (this shared box also runs Jellyfin/media services).
#   MF_MEM   memory cap          (default 6G)
#   MF_TIME  runtime cap seconds (default 1800)
#   MF_CPU   CPU quota percent   (default 400 = 4 cores)
#   MF_HIGH  soft limit          (default 75% of MF_MEM)
# Fails closed (exit 125) when no user systemd scope is available.
# Usage: tools/bounded.sh [--] command args...
set -u
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || { echo "usage: $0 command..." >&2; exit 2; }
mem=${MF_MEM:-6G}
# Soft throttle threshold at 75% of the hard cap: reclaim/throttle starts before the kill.
high=${MF_HIGH:-$(echo "$mem" | awk '{n=$0; sub(/[A-Za-z]+$/,"",n); u=substr($0,length(n)+1); printf "%d%s", n*3/4*(u=="G"?1024:1), (u=="G"?"M":u)}')}
# Fail closed: without a working cgroup scope there is no memory containment, so refuse.
if ! command -v systemd-run >/dev/null 2>&1 || ! systemd-run --user --scope -q true 2>/dev/null; then
    echo "bounded.sh: user systemd scope unavailable; refusing to run heavy job without memory containment" >&2
    exit 125
fi
exec systemd-run --user --scope --quiet --collect \
    -p MemoryMax="$mem" -p MemoryHigh="$high" -p MemorySwapMax=0 -p TasksMax=4096 \
    -p CPUQuota="${MF_CPU:-400}%" -p RuntimeMaxSec="${MF_TIME:-1800}" -- "$@"
