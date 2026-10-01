#!/bin/sh
# Offline coverage for device-run.sh --no-session / the login-connect scenario.
# No device, SSH, network, or production application is involved.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
LIB="$SCRIPT_DIR/no-session-surgery.sh"
BAK=session.txt.uiscript-bak
TMP_ROOT=$(mktemp -d /tmp/uiscript-no-session.XXXXXX)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() { echo "no-session test failed: $*" >&2; exit 1; }

[ -f "$LIB" ] || fail 'surgery helper is missing'
sh -n "$LIB" || fail 'surgery helper has invalid syntax'
sh -n "$SCRIPT_DIR/device-run.sh" || fail 'device-run.sh has invalid syntax'
[ -f "$SCRIPT_DIR/scripts/login-connect-device.txt" ] \
    || fail 'login-connect device script is missing'

# shellcheck disable=SC1090
. "$LIB"

new_fixture() {
    dir="$TMP_ROOT/$1"
    mkdir -p "$dir"
    printf 'server_url=http://x.test\naccess_token=SECRET_TOKEN_VALUE\nuser_id=u1\n' >"$dir/session.txt"
    cp "$dir/session.txt" "$dir/session.orig"
    echo "$dir"
}

# --- apply blanks session.txt, keeps a verified backup, never prints it -----
D=$(new_fixture roundtrip)
INODE=$(stat -c %i "$D/session.txt")
OUT=$(miyoofin_no_session_apply "$D" "$BAK" 2>&1) || fail 'apply failed'
[ ! -s "$D/session.txt" ] || fail 'session.txt was not blanked'
cmp -s "$D/$BAK" "$D/session.orig" || fail 'backup is not byte-identical to the original'
echo "$OUT" | grep -q SECRET_TOKEN_VALUE && fail 'apply printed the token' || true
[ "$(stat -c %i "$D/session.txt")" = "$INODE" ] || fail 'apply replaced the session.txt inode'

# --- stale backup is refused and nothing is clobbered -----------------------
if miyoofin_no_session_apply "$D" "$BAK" >/dev/null 2>&1; then
    fail 'apply accepted a stale backup'
fi
cmp -s "$D/$BAK" "$D/session.orig" || fail 'stale-backup refusal damaged the backup'

# --- restore is byte-identical, keeps the inode, removes only the backup ----
OUT=$(miyoofin_no_session_restore "$D" "$BAK" 2>&1) || fail 'restore failed'
cmp -s "$D/session.txt" "$D/session.orig" || fail 'restored session.txt differs from the original'
[ ! -e "$D/$BAK" ] || fail 'restore left the backup behind'
echo "$OUT" | grep -q SECRET_TOKEN_VALUE && fail 'restore printed the token' || true
[ "$(stat -c %i "$D/session.txt")" = "$INODE" ] || fail 'restore replaced the session.txt inode'

# --- restore with no backup refuses and leaves session.txt untouched --------
D=$(new_fixture nobackup)
if miyoofin_no_session_restore "$D" "$BAK" >/dev/null 2>&1; then
    fail 'restore succeeded without a backup'
fi
cmp -s "$D/session.txt" "$D/session.orig" || fail 'restore without backup modified session.txt'

# --- apply without session.txt refuses --------------------------------------
D="$TMP_ROOT/nosession"
mkdir -p "$D"
if miyoofin_no_session_apply "$D" "$BAK" >/dev/null 2>&1; then
    fail 'apply succeeded without session.txt'
fi
[ ! -e "$D/$BAK" ] || fail 'apply created a backup without a session'

# --- a failed cp leaves no backup (restore can never clobber with a partial) -
D=$(new_fixture failedcp)
cp() { return 1; }
if miyoofin_no_session_apply "$D" "$BAK" >/dev/null 2>&1; then
    unset -f cp
    fail 'apply succeeded despite a failed backup copy'
fi
unset -f cp
[ ! -e "$D/$BAK" ] || fail 'failed backup left a partial file'
cmp -s "$D/session.txt" "$D/session.orig" || fail 'failed backup modified session.txt'
if miyoofin_no_session_restore "$D" "$BAK" >/dev/null 2>&1; then
    fail 'restore ran after a failed backup'
fi
cmp -s "$D/session.txt" "$D/session.orig" || fail 'restore after failed backup modified session.txt'

# --- device-run option parsing / dry-run, no SSH ----------------------------
sh "$SCRIPT_DIR/device-run.sh" --help 2>&1 | grep -q -- '--no-session' \
    || fail '--help does not mention --no-session'
DRY="$TMP_ROOT/dry.txt"
sh "$SCRIPT_DIR/device-run.sh" --dry-run login-connect >"$DRY" 2>&1 \
    || fail 'login-connect dry-run failed'
grep -q 'no-session' "$DRY" || fail 'login-connect does not imply --no-session'
! grep -q 'SECRET_TOKEN\|access_token' "$DRY" || fail 'dry-run printed token content'
if sh "$SCRIPT_DIR/device-run.sh" --dry-run --no-session --server-unavailable smoke >/dev/null 2>&1; then
    fail '--no-session and --server-unavailable were accepted together'
fi

echo 'no-session offline tests passed'
