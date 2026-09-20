#!/bin/sh
# Offline coverage for device-run.sh --server-unavailable.
# No device, SSH, network, or production application is involved.

set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
LIB="$SCRIPT_DIR/server-unavailable-surgery.sh"
ENDPOINT=http://127.0.0.1:9
TMP_ROOT=$(mktemp -d /tmp/uiscript-server-unavailable.XXXXXX)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() { echo "server-unavailable test failed: $*" >&2; exit 1; }

[ -f "$LIB" ] || fail 'shared configuration surgery helper is missing'
sh -n "$LIB" || fail 'shared configuration surgery helper has invalid syntax'
sh -n "$SCRIPT_DIR/device-run.sh" || fail 'device-run.sh has invalid syntax'
grep -q -- '--server-unavailable' "$SCRIPT_DIR/device-run.sh" \
    || fail 'device-run.sh does not expose --server-unavailable'
grep -q '127.0.0.1:9' "$SCRIPT_DIR/device-run.sh" \
    || fail 'device-run.sh does not use the closed loopback endpoint'
grep -Fq 'WAIT_LOG [HomeScreen] Library loaded' "$SCRIPT_DIR/scripts/smoke-device.txt" \
    || fail 'server-unavailable smoke changed the normal load marker'
grep -Fq "nonfatal_want='[Route] LAN failed; public fallback'" "$SCRIPT_DIR/device-run.sh" \
    || fail 'server-unavailable smoke does not require the nonfatal route marker'

# The help and dry-run paths prove option parsing without contacting SSH.
sh "$SCRIPT_DIR/device-run.sh" --help >/dev/null 2>&1 \
    || fail '--help rejected the new option syntax'
DRY="$TMP_ROOT/dry-run.txt"
sh "$SCRIPT_DIR/device-run.sh" --dry-run --server-unavailable smoke >"$DRY" 2>&1 \
    || fail '--server-unavailable dry-run failed'
grep -q 'server-unavailable' "$DRY" || fail 'dry-run omitted server-unavailable mode'
grep -q 'closed loopback endpoint' "$DRY" || fail 'dry-run omitted closed endpoint description'
! grep -q 'access_token\|SECRET_TOKEN' "$DRY" \
    || fail 'dry-run printed session/token content'
! grep -Eq 'https?://' "$DRY" \
    || fail 'dry-run printed a full URL'

. "$LIB"

new_fixture() {
    _fix="$TMP_ROOT/fixture-$1"
    mkdir -p "$_fix"
    printf '%s\n' 'https://real.example.invalid:8096' >"$_fix/server.txt"
    cat >"$_fix/session.txt" <<'EOF'
server_url=https://real.example.invalid:8096
server_id=server-id
local_server_url=http://lan.example.invalid:8096
public_server_url=https://public.example.invalid:443
access_token=SECRET_TOKEN_MUST_STAY_ON_DEVICE
user_id=user-id
EOF
    printf '%s' "$_fix"
}

echo '[test] apply routes server.txt and only session endpoint override keys'
FIX=$(new_fixture apply)
cp "$FIX/server.txt" "$TMP_ROOT/server.original"
cp "$FIX/session.txt" "$TMP_ROOT/session.original"
OUT="$TMP_ROOT/apply.out"
miyoofin_server_unavailable_apply "$FIX" server.txt.uiscript-bak session.txt.uiscript-bak "$ENDPOINT" >"$OUT" 2>&1 \
    || fail 'configuration apply failed'
printf '%s\n' "$ENDPOINT" | cmp -s - "$FIX/server.txt" || fail 'server.txt was not routed to the closed endpoint'
grep -q '^server_url=https://real.example.invalid:8096$' "$FIX/session.txt" \
    || fail 'server_url identity was changed'
grep -q '^local_server_url=http://127.0.0.1:9$' "$FIX/session.txt" \
    || fail 'local_server_url was not routed'
grep -q '^public_server_url=http://127.0.0.1:9$' "$FIX/session.txt" \
    || fail 'public_server_url was not routed'
grep -q '^access_token=SECRET_TOKEN_MUST_STAY_ON_DEVICE$' "$FIX/session.txt" || fail 'access_token was modified'
cmp -s "$TMP_ROOT/server.original" "$FIX/server.txt.uiscript-bak" \
    || fail 'server.txt backup was not byte-identical'
cmp -s "$TMP_ROOT/session.original" "$FIX/session.txt.uiscript-bak" \
    || fail 'session.txt backup was not byte-identical'
! grep -q 'SECRET_TOKEN' "$OUT" || fail 'apply printed token content'
! grep -Eq 'https?://' "$OUT" || fail 'apply printed a full URL'

echo '[test] trap-style restore is byte-identical and removes backups'
TRAP_DRIVER="$TMP_ROOT/trap-driver.sh"
cat >"$TRAP_DRIVER" <<EOF
#!/bin/sh
set -eu
. "$LIB"
FIX="$FIX"
cleanup() {
    rc=\$?
    miyoofin_server_unavailable_restore "\$FIX" server.txt.uiscript-bak session.txt.uiscript-bak >/dev/null
    exit "\$rc"
}
trap cleanup EXIT
exit 17
EOF
if sh "$TRAP_DRIVER" >"$TMP_ROOT/trap.out" 2>&1; then
    fail 'trap driver unexpectedly returned zero'
fi
[ "$?" -ne 0 ] 2>/dev/null || true
# The failed command above is intentionally followed by checks rather than a
# shell assertion on $?, because set -e would otherwise obscure the test.
cmp -s "$TMP_ROOT/server.original" "$FIX/server.txt" \
    || fail 'trap did not restore server.txt byte-for-byte'
cmp -s "$TMP_ROOT/session.original" "$FIX/session.txt" \
    || fail 'trap did not restore session.txt byte-for-byte'
[ ! -e "$FIX/server.txt.uiscript-bak" ] && [ ! -e "$FIX/session.txt.uiscript-bak" ] \
    || fail 'trap left configuration backups behind'
! grep -q 'SECRET_TOKEN' "$TMP_ROOT/trap.out" \
    || fail 'restore diagnostics printed token content'
rm -rf "$FIX"

echo '[test] apply adds missing endpoint overrides without changing identity'
FIX=$(new_fixture sparse)
awk '!/^(local_server_url|public_server_url)=/' "$FIX/session.txt" >"$FIX/session.txt.new"
mv "$FIX/session.txt.new" "$FIX/session.txt"
cp "$FIX/server.txt" "$TMP_ROOT/sparse-server.original"
cp "$FIX/session.txt" "$TMP_ROOT/sparse-session.original"
miyoofin_server_unavailable_apply "$FIX" server.txt.uiscript-bak session.txt.uiscript-bak "$ENDPOINT" >/dev/null 2>&1 \
    || fail 'configuration apply with missing endpoint overrides failed'
grep -q '^server_url=https://real.example.invalid:8096$' "$FIX/session.txt" \
    || fail 'sparse session server_url identity was changed'
grep -q '^local_server_url=http://127.0.0.1:9$' "$FIX/session.txt" \
    || fail 'missing local_server_url was not added'
grep -q '^public_server_url=http://127.0.0.1:9$' "$FIX/session.txt" \
    || fail 'missing public_server_url was not added'
cmp -s "$TMP_ROOT/sparse-server.original" "$FIX/server.txt.uiscript-bak" \
    || fail 'sparse server.txt backup was not byte-identical'
cmp -s "$TMP_ROOT/sparse-session.original" "$FIX/session.txt.uiscript-bak" \
    || fail 'sparse session.txt backup was not byte-identical'
miyoofin_server_unavailable_restore "$FIX" server.txt.uiscript-bak session.txt.uiscript-bak >/dev/null 2>&1 \
    || fail 'sparse configuration restore failed'
cmp -s "$TMP_ROOT/sparse-server.original" "$FIX/server.txt" \
    || fail 'sparse server.txt was not restored byte-for-byte'
cmp -s "$TMP_ROOT/sparse-session.original" "$FIX/session.txt" \
    || fail 'sparse session.txt was not restored byte-for-byte'
[ ! -e "$FIX/server.txt.uiscript-bak" ] && [ ! -e "$FIX/session.txt.uiscript-bak" ] \
    || fail 'sparse restore left configuration backups behind'
rm -rf "$FIX"

echo '[test] restore retries safely after a halfway failure'
FIX=$(new_fixture retry)
cp "$FIX/server.txt" "$TMP_ROOT/retry-server.original"
cp "$FIX/session.txt" "$TMP_ROOT/retry-session.original"
miyoofin_server_unavailable_apply "$FIX" server.txt.uiscript-bak session.txt.uiscript-bak "$ENDPOINT" >/dev/null 2>&1
rm -f "$FIX/session.txt.uiscript-bak"
if miyoofin_server_unavailable_restore "$FIX" server.txt.uiscript-bak session.txt.uiscript-bak >/dev/null 2>&1; then
    fail 'restore accepted a missing backup'
fi
[ -e "$FIX/server.txt.uiscript-bak" ] || fail 'halfway restore removed valid backup'
rm -rf "$FIX"

echo '[test] server-unavailable configuration coverage OK'
