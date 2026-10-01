#!/bin/sh
# End-to-end test of `miyoofin-playback-reporter <app-dir> --refresh-resume`
# against a loopback stub Jellyfin server. No network beyond 127.0.0.1.
#
# Remote playback must resume from the SERVER's position (it may have moved on
# another device); every failure path must leave the cached resume_ticks alone.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
REPORTER=${REPORTER:-$ROOT/output/build/miyoofin-playback-reporter}
[ -x "$REPORTER" ] || { echo "resume-refresh test: build the reporter first (make reporter)" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "resume-refresh test: python3 required" >&2; exit 1; }

TMP=$(mktemp -d "${TMPDIR:-/tmp}/miyoofin-resume-refresh.XXXXXX")
STUB_PID=
cleanup() {
    [ -z "$STUB_PID" ] || kill "$STUB_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

fail() { echo "resume-refresh test failed: $*" >&2; exit 1; }

# Stub: serves /Users/u1/Items/<id>. Position and status come from the id:
#   moved  -> PlaybackPositionTicks 24000000000   same -> 500
#   fresh  -> UserData without a position (not started)
#   bad    -> 500        nobody -> 404           junk -> 200 with no UserData
cat >"$TMP/stub.py" <<'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        item = self.path.rsplit("/", 1)[-1]
        if not self.headers.get("X-Emby-Token"):
            self.send_response(401); self.end_headers(); return
        if item == "bad": self.send_response(500); self.end_headers(); return
        if item == "nobody": self.send_response(404); self.end_headers(); return
        if item == "junk": body = b'{"Name":"x"}'
        elif item == "fresh": body = b'{"UserData":{"Played":false}}'
        elif item == "same": body = b'{"UserData":{"PlaybackPositionTicks":500}}'
        else: body = b'{"UserData":{"PlaybackPositionTicks":24000000000,"PlayCount":1}}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 "$TMP/stub.py" >"$TMP/port.txt" &
STUB_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$TMP/port.txt" ] && break; sleep 0.3; done
PORT=$(cat "$TMP/port.txt")
[ -n "$PORT" ] || fail 'stub server did not start'

# make_app NAME ITEM MODE TICKS [PORT] -> app dir with session + request files.
make_app() {
    dir="$TMP/$1"
    mkdir -p "$dir"
    printf 'server_url=http://127.0.0.1:%s\naccess_token=SECRET_TOKEN\nuser_id=u1\ndevice_id=dev1\n' "${5:-$PORT}" >"$dir/session.txt"
    printf 'item_id=%s\nitem_type=movie\nresume_ticks=%s\nsource_mode=%s\n' "$2" "$4" "$3" >"$dir/playback-request.txt"
    echo "$dir"
}
ticks_of() { grep '^resume_ticks=' "$1/playback-request.txt" | cut -d= -f2; }

# 1. The server moved on (watched on another device): ticks are replaced.
d=$(make_app moved moved jellyfin 100)
out=$("$REPORTER" "$d" --refresh-resume)
[ "$(ticks_of "$d")" = 24000000000 ] || fail "stale ticks were not refreshed (got $(ticks_of "$d"))"
echo "$out" | grep -q 'resume_refresh updated old=100 new=24000000000' || fail "unexpected output: $out"
grep -q '^item_id=moved$' "$d/playback-request.txt" || fail 'other request lines were damaged'
grep -q '^source_mode=jellyfin$' "$d/playback-request.txt" || fail 'source_mode was damaged'
! echo "$out" | grep -q SECRET_TOKEN || fail 'output leaked the token'

# 2. Already in sync: reported unchanged, file untouched.
d=$(make_app same same jellyfin 500)
out=$("$REPORTER" "$d" --refresh-resume)
echo "$out" | grep -q 'resume_refresh unchanged ticks=500' || fail "unexpected output: $out"
[ "$(ticks_of "$d")" = 500 ] || fail 'unchanged case rewrote ticks'

# 3. Not started on the server (no position): the server is authoritative, 0.
d=$(make_app fresh fresh jellyfin 777)
"$REPORTER" "$d" --refresh-resume >/dev/null
[ "$(ticks_of "$d")" = 0 ] || fail "server 'not started' did not reset ticks (got $(ticks_of "$d"))"

# 4. Failures keep the cached position: 500, 404, junk body, server down.
for item in bad nobody junk; do
    d=$(make_app "fail_$item" "$item" jellyfin 4242)
    out=$("$REPORTER" "$d" --refresh-resume)
    [ "$(ticks_of "$d")" = 4242 ] || fail "$item: failure changed the cached ticks"
    echo "$out" | grep -q 'resume_refresh failed' || fail "$item: expected a failed line, got: $out"
done
d=$(make_app down moved jellyfin 4242 1)
out=$("$REPORTER" "$d" --refresh-resume)
[ "$(ticks_of "$d")" = 4242 ] || fail 'unreachable server changed the cached ticks'
echo "$out" | grep -q 'resume_refresh failed' || fail "down: expected a failed line, got: $out"

# 5. Downloaded playback is never refreshed from the server.
d=$(make_app local moved local 100)
out=$("$REPORTER" "$d" --refresh-resume)
[ "$(ticks_of "$d")" = 100 ] || fail 'local playback was refreshed'
echo "$out" | grep -q 'skipped reason=local' || fail "local: unexpected output: $out"

# 6. Unsafe ids are rejected before any request, and no session is a no-op.
d=$(make_app unsafe 'a/../b' jellyfin 100)
out=$("$REPORTER" "$d" --refresh-resume)
[ "$(ticks_of "$d")" = 100 ] || fail 'unsafe id changed ticks'
echo "$out" | grep -q 'skipped reason=no_session_or_bad_id' || fail "unsafe: unexpected output: $out"
d=$(make_app nosession moved jellyfin 100)
rm -f "$d/session.txt"
"$REPORTER" "$d" --refresh-resume >/dev/null
[ "$(ticks_of "$d")" = 100 ] || fail 'missing session changed ticks'

# 7. HTTPS without a CA bundle never weakens verification: skipped, unchanged.
d=$(make_app noca moved jellyfin 100)
printf 'server_url=https://127.0.0.1:%s\naccess_token=t\nuser_id=u1\ndevice_id=dev1\n' "$PORT" >"$d/session.txt"
out=$("$REPORTER" "$d" --refresh-resume)
[ "$(ticks_of "$d")" = 100 ] || fail 'https without CA changed ticks'
echo "$out" | grep -q 'skipped reason=no_ca' || fail "noca: unexpected output: $out"

# 8. The normal (single-argument) usage is unchanged: a bare unknown 2nd arg fails.
"$REPORTER" "$TMP/moved" --bogus >/dev/null 2>&1 && fail 'unknown flag was accepted' || true

echo "[test] playback resume refresh OK"
