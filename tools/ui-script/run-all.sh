#!/bin/sh
# Runs the offline harness tests and the host UI flows (make ui-script-test).
#
# The UI flows share a /tmp screenshot flag file, so they stay sequential. The
# remote-scenario test is offline and shares nothing with them, so it runs
# alongside the flows instead of after them (about 12s saved).
set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd)
repo_root=$(CDPATH= cd "$script_dir/../.." && pwd)
cd "$repo_root"
mkdir -p output/ui-script
remote_log=output/ui-script/remote-scenario.log

# Fast offline tests first: they fail fast.
sh tools/ui-script/test-server-unavailable.sh
sh tools/ui-script/test-launcher-roundtrip.sh

sh tools/ui-script/test-remote-scenario.sh >"$remote_log" 2>&1 &
remote_pid=$!

status=0
for flow in smoke movies series login-400; do
    sh tools/ui-script/run.sh "$flow" || { status=$?; break; }
done

remote_status=0
wait "$remote_pid" || remote_status=$?
cat "$remote_log"
if [ "$remote_status" -ne 0 ]; then
    echo "ui-script: test-remote-scenario.sh failed (status $remote_status)" >&2
    [ "$status" -ne 0 ] || status=$remote_status
fi
exit "$status"
