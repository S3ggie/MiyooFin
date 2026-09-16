#!/bin/sh
# MiyooFin headless UI-harness runner (test-only, host/desktop only).
#
# Runs one scripted UI flow against the host build with no Miyoo hardware
# and no real Jellyfin server:
#   1. builds the host binary (if needed) and the LD_PRELOAD input shim,
#   2. starts a loopback stub Jellyfin server (tools/ui-script/stub_server.py),
#   3. seeds an isolated runtime dir (cwd) with a stub session.txt,
#   4. runs the app under Xvfb (CI) or the current display with the shim
#      replaying tools/ui-script/scripts/<name>.txt,
#   5. collects framebuffer BMPs + app log into output/ui-script/<name>/,
#   6. runs the coarse screenshot assertions; exits non-zero on any failure.
#
# Usage: sh tools/ui-script/run.sh <smoke|series>
# Screenshots land in output/ui-script/<name>/ (checked-in? no: gitignored
# output/ tree). Per-script log-marker + pixel expectations live below.

set -eu

NAME=${1:?usage: run.sh '<smoke|series>'}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SCRIPT_DIR="$ROOT/tools/ui-script"
SCRIPT="$SCRIPT_DIR/scripts/$NAME.txt"
OUT="$ROOT/output/ui-script/$NAME"
SHIM_SRC="$SCRIPT_DIR/shim.cpp"
SHIM_SO="$ROOT/output/ui-script/shim.so"
BIN="$ROOT/output/build/miyoofin"

[ -f "$SCRIPT" ] || { echo "ui-script: no such script: $SCRIPT" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ui-script: python3 required" >&2; exit 2; }

fail() { echo "ui-script($NAME): $*" >&2; exit 1; }

# --- 1. build host binary + shim -----------------------------------------
(cd "$ROOT" && make -j2 >/dev/null) || fail "host build failed"
[ -x "$BIN" ] || fail "host binary missing after build: $BIN"
if [ ! -f "$SHIM_SO" ] || [ "$SHIM_SRC" -nt "$SHIM_SO" ]; then
    mkdir -p "$(dirname "$SHIM_SO")"
    SDL_FLAGS=$(pkg-config --cflags sdl2 2>/dev/null || echo '-I/usr/include/SDL2')
    # shellcheck disable=SC2086
    g++ -shared -fPIC -O1 -Wall -Wextra $SDL_FLAGS \
        -o "$SHIM_SO" "$SHIM_SRC" -ldl || fail "shim build failed"
    echo "ui-script: built $SHIM_SO"
fi

# --- 2. isolated runtime dir + stub server --------------------------------
RUNDIR=$(mktemp -d /tmp/miyoofin-ui-script.XXXXXX)
STUB_PID=""
cleanup() {
    if [ -n "$STUB_PID" ] && kill -0 "$STUB_PID" 2>/dev/null; then
        kill "$STUB_PID" 2>/dev/null || true
        wait "$STUB_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNDIR"
}
trap cleanup EXIT INT TERM

python3 "$SCRIPT_DIR/stub_server.py" "$RUNDIR/port" >"$RUNDIR/stub.log" 2>&1 &
STUB_PID=$!
PORT=""
i=0
while [ $i -lt 100 ]; do
    if [ -s "$RUNDIR/port" ]; then PORT=$(cat "$RUNDIR/port"); break; fi
    if ! kill -0 "$STUB_PID" 2>/dev/null; then
        sed 's/^/stub: /' "$RUNDIR/stub.log" >&2 || true
        fail "stub server died on startup"
    fi
    sleep 0.1
    i=$((i + 1))
done
[ -n "$PORT" ] || fail "stub server did not report a port"
echo "ui-script: stub Jellyfin on 127.0.0.1:$PORT"

# Seeded session: valid token/user against the stub only. Runs entirely on
# loopback; no real server, no credentials, no network.
cat >"$RUNDIR/session.txt" <<EOF
server_url=http://127.0.0.1:$PORT
access_token=stub-token
user_id=user-stub
user_name=stub
manual_offline_mode=0
EOF
chmod 600 "$RUNDIR/session.txt"

# --- 3. run the app with the input shim -----------------------------------
mkdir -p "$OUT/shots"
APPLOG="$OUT/app.log"
: >"$APPLOG"
: >"$OUT/result.txt"

# Xvfb when there is no display (CI); the current display otherwise.
# Override by exporting XVFB_RUN explicitly (empty string forces direct).
if [ -z "${XVFB_RUN+x}" ]; then
    if [ -z "${DISPLAY:-}" ] && command -v xvfb-run >/dev/null 2>&1; then
        XVFB_RUN="xvfb-run -a"
    else
        XVFB_RUN=""
    fi
fi

# NOTE: `exec` so $! is the app itself, not a subshell — the watchdog's
# kill must reach the app, or a hung run orphans it and floods the logs.
(
    cd "$RUNDIR"
    # shellcheck disable=SC2086
    exec env \
        MIYOOFIN_DESKTOP_INPUT=1 \
        MIYOOFIN_DESKTOP_WINDOW=1 \
        MIYOOFIN_PLAYBACK_MODE=desktop \
        LD_PRELOAD="$SHIM_SO" \
        MIYOOFIN_UI_SCRIPT="$SCRIPT" \
        MIYOOFIN_UI_LOG="$APPLOG" \
        MIYOOFIN_UI_SHOT_DIR="$OUT/shots" \
        MIYOOFIN_UI_RESULT="$OUT/result.txt" \
        $XVFB_RUN "$BIN" >"$APPLOG" 2>&1
) &
APP_PID=$!

# Bounded watchdog: the script ends with QUIT (clean exit 0). Anything else
# is a hang — kill and fail loudly instead of blocking CI forever.
TIMEOUT_S=${MIYOOFIN_UI_TIMEOUT_S:-120}
elapsed=0
while kill -0 "$APP_PID" 2>/dev/null; do
    if [ "$elapsed" -ge "$TIMEOUT_S" ]; then
        kill -9 "$APP_PID" 2>/dev/null || true
        fail "app did not exit within ${TIMEOUT_S}s (script QUIT missing?)"
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done
wait "$APP_PID" 2>/dev/null && APP_RC=0 || APP_RC=$?
[ "$APP_RC" -eq 0 ] || fail "app exited with status $APP_RC (see $APPLOG)"
# Keep the stub request log beside the app log for failed-run debugging.
cp "$RUNDIR/stub.log" "$OUT/stub.log" 2>/dev/null || true

# --- 4. assertions ---------------------------------------------------------
# a) shim-level failures (WAIT_LOG/SCREENSHOT timeouts, bad script lines)
if grep -q '^FAIL' "$OUT/result.txt" 2>/dev/null; then
    grep '^FAIL' "$OUT/result.txt" | sed 's/^/ui-script: /' >&2
    fail "script driver reported failure"
fi

case "$NAME" in
    smoke)
        want='[HomeScreen] Library loaded'
        SHOT="$OUT/shots/home.bmp"
        CHECKS="rendered,rails"
        ;;
    series)
        want='[SeriesScreen] enter series=Testville'
        SHOT="$OUT/shots/series-seasons.bmp"
        CHECKS="rendered,seasons"
        ;;
    *) fail "no expectations defined for script '$NAME'" ;;
esac

grep -Fq "$want" "$APPLOG" \
    || fail "expected screen marker missing from log: $want"
echo "ui-script: log marker OK: $want"

[ -f "$SHOT" ] || fail "expected screenshot missing: $SHOT"
python3 "$SCRIPT_DIR/assert_shots.py" --shot "$SHOT" --checks "$CHECKS" \
    || fail "screenshot assertions failed for $SHOT"

echo "ui-script($NAME): PASS (shots in $OUT/shots, log in $APPLOG)"
