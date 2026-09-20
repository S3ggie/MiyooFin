#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
RUNNER="$ROOT/distributions/onionos/playback_runner.sh"
TMP_ROOT=$(mktemp -d /tmp/miyoofin-runner-test.XXXXXX)
LIFECYCLE_RUNNER_PID=
LIFECYCLE_FFPLAY_PID=
LIFECYCLE_REPORTER_PID=
LIFECYCLE_BRIDGE_PID=
cleanup() {
    if [ -n "$LIFECYCLE_RUNNER_PID" ] && kill -0 "$LIFECYCLE_RUNNER_PID" 2>/dev/null; then
        kill -TERM "$LIFECYCLE_RUNNER_PID" 2>/dev/null || true
        wait "$LIFECYCLE_RUNNER_PID" 2>/dev/null || true
    fi
    if [ -n "$LIFECYCLE_FFPLAY_PID" ] && kill -0 "$LIFECYCLE_FFPLAY_PID" 2>/dev/null; then
        kill -KILL "$LIFECYCLE_FFPLAY_PID" 2>/dev/null || true
        wait "$LIFECYCLE_FFPLAY_PID" 2>/dev/null || true
    fi
    if [ -n "$LIFECYCLE_REPORTER_PID" ] && kill -0 "$LIFECYCLE_REPORTER_PID" 2>/dev/null; then
        kill -KILL "$LIFECYCLE_REPORTER_PID" 2>/dev/null || true
        wait "$LIFECYCLE_REPORTER_PID" 2>/dev/null || true
    fi
    if [ -n "$LIFECYCLE_BRIDGE_PID" ] && kill -0 "$LIFECYCLE_BRIDGE_PID" 2>/dev/null; then
        kill -KILL "$LIFECYCLE_BRIDGE_PID" 2>/dev/null || true
        wait "$LIFECYCLE_BRIDGE_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT HUP INT TERM

fail() { echo "runner test failed: $*" >&2; exit 1; }

run_case() {
    case_dir=$1
    session=$2
    with_ca=$3
    app_dir="$TMP_ROOT/$case_dir"
    mkdir -p "$app_dir"
    cp "$RUNNER" "$app_dir/playback_runner.sh"
    printf '%s\n' 'item_id=item' 'item_type=movie' 'resume_ticks=0' 'source_mode=jellyfin' > "$app_dir/playback-request.txt"
    printf '%s\n' "$session" 'access_token=token' > "$app_dir/session.txt"
    if [ "$with_ca" = yes ]; then printf '%s\n' '-----BEGIN CERTIFICATE-----' 'test' '-----END CERTIFICATE-----' > "$app_dir/cacert.pem"; fi
    cat > "$app_dir/miyoofin-https-bridge" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$(dirname "$0")/bridge-args.txt"
exit 0
EOF
    chmod +x "$app_dir/miyoofin-https-bridge"
    (cd "$app_dir" && sh ./playback_runner.sh) >/dev/null 2>&1 || true
}

# HTTP-only routes pass an empty CA argument and launch normally.
run_case http_only 'server_url=http://192.168.1.212:8096' no
[ -f "$TMP_ROOT/http_only/bridge-args.txt" ] || fail 'HTTP-only route did not launch bridge'
grep -q '^http://192.168.1.212:8096/' "$TMP_ROOT/http_only/bridge-args.txt" || fail 'HTTP-only route did not use HTTP upstream'
[ "$(sed -n '2p' "$TMP_ROOT/http_only/bridge-args.txt")" = "" ] || fail 'HTTP-only route passed a CA path'
[ "$(sed -n '3p' "$TMP_ROOT/http_only/bridge-args.txt")" = 18080 ] || fail 'HTTP-only route passed unexpected port'

# LAN HTTP with an HTTPS fallback launches LAN only when CA data is absent.
run_case lan_fallback 'server_url=http://192.168.1.212:8096
public_server_url=https://jellyfin.example.com' no
[ -f "$TMP_ROOT/lan_fallback/bridge-args.txt" ] || fail 'LAN route did not launch bridge'
! grep -q -- '--fallback-url\|https://jellyfin.example.com' "$TMP_ROOT/lan_fallback/bridge-args.txt" || fail 'HTTPS fallback was not disabled'
grep -q 'Secure HTTPS fallback unavailable: cacert.pem not found' "$TMP_ROOT/lan_fallback/playback-launch.log" || fail 'missing fallback warning'
[ "$(sed -n '2p' "$TMP_ROOT/lan_fallback/bridge-args.txt")" = "" ] || fail 'LAN-only launch passed a CA path'

# HTTPS-only playback is rejected before the bridge starts without CA data.
run_case https_only 'server_url=https://jellyfin.example.com' no
! [ -f "$TMP_ROOT/https_only/bridge-args.txt" ] || fail 'HTTPS-only route launched without CA'
grep -q 'ERROR: cacert.pem not found for HTTPS playback' "$TMP_ROOT/https_only/playback-launch.log" || fail 'missing HTTPS-only rejection'

# An HTTPS route retains the CA argument when it is available.
run_case https_with_ca 'server_url=https://jellyfin.example.com' yes
[ "$(sed -n '2p' "$TMP_ROOT/https_with_ca/bridge-args.txt")" = "$TMP_ROOT/https_with_ca/cacert.pem" ] || fail 'HTTPS route did not pass CA path'

echo '[test] playback runner route-aware CA handling OK'

# Onion FFplay selects its native drivers after MiyooFin releases SDL.  Do not
# force the SDL2 driver names inherited by the application onto the player.
! grep -q 'export SDL_VIDEODRIVER=mmiyoo' "$RUNNER" || fail 'runner forces MiyooFin video driver onto FFplay'
! grep -q 'export SDL_AUDIODRIVER=mmiyoo' "$RUNNER" || fail 'runner forces MiyooFin audio driver onto FFplay'
grep -q 'PLAYBACK_FFPLAY_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so' "$RUNNER" || fail 'Onion audio bridge path is missing'
grep -q 'LD_PRELOAD="$PLAYBACK_FFPLAY_PRELOAD" ./bin/ffplay \\' "$RUNNER" || fail 'FFplay does not use Onion audio bridge'
! grep -q 'ffplay_argv=.*\$PLAY_URL' "$RUNNER" || fail 'FFplay argv diagnostic leaks input URL'

echo '[test] playback runner Onion native SDL and DSP audio setup OK'

# A supported app exit terminates the runner while it is blocked in FFplay.
# The runner must clean up its exact external children rather than leaving an
# orphaned player behind after the parent has reaped it.
LIFECYCLE_DIR="$TMP_ROOT/forced-exit"
mkdir -p "$LIFECYCLE_DIR"
cp "$RUNNER" "$LIFECYCLE_DIR/playback_runner.sh"
printf '%s\n' 'item_id=item' 'item_type=movie' 'resume_ticks=0' 'source_mode=jellyfin' > "$LIFECYCLE_DIR/playback-request.txt"
printf '%s\n' 'server_url=http://127.0.0.1:8096' 'access_token=token' > "$LIFECYCLE_DIR/session.txt"
cat > "$LIFECYCLE_DIR/miyoofin-https-bridge" <<'EOF'
#!/bin/sh
printf '%s\n' "$$" > "$(dirname "$0")/bridge-shell.pid"
trap '' TERM INT HUP
while :; do :; done
EOF
cat > "$LIFECYCLE_DIR/miyoofin-playback-reporter" <<'EOF'
#!/bin/sh
printf '%s\n' "$$" > "$(dirname "$0")/reporter-shell.pid"
trap '' TERM INT HUP
while :; do :; done
EOF
cat > "$LIFECYCLE_DIR/fake-ffplay" <<'EOF'
#!/bin/sh
printf '%s\n' "$$" > "$(dirname "$0")/ffplay.pid"
trap 'printf exited > "$(dirname "$0")/ffplay-exited"; exit 0' TERM INT HUP
while :; do sleep 1; done
EOF
chmod +x "$LIFECYCLE_DIR/miyoofin-https-bridge" \
    "$LIFECYCLE_DIR/miyoofin-playback-reporter" "$LIFECYCLE_DIR/fake-ffplay"
(cd "$LIFECYCLE_DIR" && \
    MIYOOFIN_PLAYBACK_MODE=desktop MIYOOFIN_FFPLAY_BIN="$LIFECYCLE_DIR/fake-ffplay" \
    sh ./playback_runner.sh) >"$LIFECYCLE_DIR/runner.log" 2>&1 &
LIFECYCLE_RUNNER_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -f "$LIFECYCLE_DIR/ffplay.pid" ] && break
    sleep 1
done
[ -s "$LIFECYCLE_DIR/ffplay.pid" ] || fail 'forced-exit case did not start fake FFplay'
LIFECYCLE_FFPLAY_PID=$(cat "$LIFECYCLE_DIR/ffplay.pid")
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -s "$LIFECYCLE_DIR/reporter-shell.pid" ] && \
        [ -s "$LIFECYCLE_DIR/bridge-shell.pid" ] && break
    sleep 1
done
[ -s "$LIFECYCLE_DIR/reporter-shell.pid" ] || fail 'forced-exit case did not start fake reporter'
[ -s "$LIFECYCLE_DIR/bridge-shell.pid" ] || fail 'forced-exit case did not record fake bridge PID'
LIFECYCLE_REPORTER_PID=$(cat "$LIFECYCLE_DIR/reporter-shell.pid")
LIFECYCLE_BRIDGE_PID=$(cat "$LIFECYCLE_DIR/bridge-shell.pid")
LIFECYCLE_START_TIME=$(date +%s)
kill -TERM "$LIFECYCLE_RUNNER_PID"
wait "$LIFECYCLE_RUNNER_PID" 2>/dev/null || true
LIFECYCLE_RUNNER_PID=
[ "$(( $(date +%s) - LIFECYCLE_START_TIME ))" -lt 10 ] \
    || fail 'forced-exit cleanup was not bounded'
[ ! -e "/proc/$LIFECYCLE_FFPLAY_PID" ] || fail 'forced runner exit left FFplay alive'
[ ! -e "/proc/$LIFECYCLE_REPORTER_PID" ] || fail 'forced runner exit left reporter alive'
[ ! -e "/proc/$LIFECYCLE_BRIDGE_PID" ] || fail 'forced runner exit left bridge alive'
[ -f "$LIFECYCLE_DIR/ffplay-exited" ] || fail 'runner did not gracefully terminate FFplay'
grep -q "ffplay_reaped pid=$LIFECYCLE_FFPLAY_PID" "$LIFECYCLE_DIR/playback-launch.log" \
    || fail 'runner did not record exact FFplay cleanup/reap evidence'
grep -q "bridge did not exit after TERM; sending KILL pid=$LIFECYCLE_BRIDGE_PID" \
    "$LIFECYCLE_DIR/playback-launch.log" \
    || fail 'runner did not KILL the exact ignored bridge PID'
grep -q "bridge_reaped pid=$LIFECYCLE_BRIDGE_PID" "$LIFECYCLE_DIR/playback-launch.log" \
    || fail 'runner did not reap the exact bridge PID'
grep -q "reporter did not exit after TERM; sending KILL pid=$LIFECYCLE_REPORTER_PID" \
    "$LIFECYCLE_DIR/playback-launch.log" \
    || fail 'runner did not KILL the exact ignored reporter PID'
grep -q "reporter_reaped pid=$LIFECYCLE_REPORTER_PID" "$LIFECYCLE_DIR/playback-launch.log" \
    || fail 'runner did not reap the exact reporter PID'
echo "[test] forced-exit cleanup evidence: ffplay pid=$LIFECYCLE_FFPLAY_PID, reporter pid=$LIFECYCLE_REPORTER_PID, bridge pid=$LIFECYCLE_BRIDGE_PID all cleaned"
LIFECYCLE_FFPLAY_PID=
LIFECYCLE_REPORTER_PID=
LIFECYCLE_BRIDGE_PID=

echo '[test] playback runner forced-exit child cleanup/reap OK'
