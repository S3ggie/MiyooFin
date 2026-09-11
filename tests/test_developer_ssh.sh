#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SCRIPT="$ROOT/tools/miyoo/miyoofin-dropbear-2222.sh"
grep -q 'SFTP_SERVER=\$BASE/bin/sftp-server' "$SCRIPT"
grep -q 'missing SFTP server' "$SCRIPT"
grep -q '\$DAEMON" -s -p 2222' "$SCRIPT"
grep -q '\$PID_FILE" -e' "$SCRIPT"
! grep -q '\$PID_FILE" -E' "$SCRIPT"
grep -q 'chmod 666 /dev/null' "$SCRIPT"
! grep -q 'port 22' "$SCRIPT"
echo 'developer SSH static checks: PASS'
