#!/bin/sh
# Optional local helper for reproducing the CI scripted UI job.
# It does not add an Xvfb dependency to normal development.

set -eu

if command -v xvfb-run >/dev/null 2>&1; then
    exec xvfb-run -a make ui-script-test
fi

echo "ui-script: xvfb-run not found; running without an Xvfb wrapper" >&2
exec make ui-script-test
