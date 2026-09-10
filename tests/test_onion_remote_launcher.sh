#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
LAUNCHER="$ROOT/tools/miyoo/onion-remote-launch.sh"
TMP_ROOT=$(mktemp -d /tmp/miyoofin-onion-launch-test.XXXXXX)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() { echo "Onion remote launcher test failed: $*" >&2; exit 1; }

[ -f "$LAUNCHER" ] || fail 'launcher is missing'
sh -n "$LAUNCHER" || fail 'launcher has invalid shell syntax'
awk '/<<'"'"'REMOTE_SCRIPT'"'"'/{capture=1; next} capture && /^REMOTE_SCRIPT$/{exit} capture{print}' \
    "$LAUNCHER" > "${TMP_ROOT}/remote-script.sh"
sh -n "${TMP_ROOT}/remote-script.sh" || fail 'embedded device script has invalid shell syntax'

FAKE_SSH="$TMP_ROOT/ssh"
cat > "$FAKE_SSH" <<'EOF'
#!/bin/sh
set -eu
printf '%s\n' "$*" > "${ONION_TEST_ARGS:?}"
cat > "${ONION_TEST_CAPTURE:?}"
EOF
chmod +x "$FAKE_SSH"

capture_mode() {
    mode=$1
    capture="$TMP_ROOT/$mode.remote"
    args="$TMP_ROOT/$mode.args"
    if [ "$mode" = telemetry ]; then
        MIYOO_SSH_TARGET=test-target PATH="$TMP_ROOT:$PATH" ONION_TEST_CAPTURE="$capture" ONION_TEST_ARGS="$args" \
            "$LAUNCHER" --telemetry >/dev/null 2>&1 || true
    else
        MIYOO_SSH_TARGET=test-target PATH="$TMP_ROOT:$PATH" ONION_TEST_CAPTURE="$capture" ONION_TEST_ARGS="$args" \
            "$LAUNCHER" >/dev/null 2>&1 || true
    fi
    [ -s "$capture" ] || fail "$mode mode did not send a remote script"
    [ -s "$args" ] || fail "$mode mode did not pass remote arguments"
    ! grep -Eq 'launch\.sh|miyoofin' "$args" || fail "$mode mode directly launched the app over SSH"
    if [ "$mode" = telemetry ]; then
        grep -q -- '-- telemetry' "$args" || fail 'telemetry mode did not pass telemetry setting'
    else
        ! grep -q -- '-- telemetry' "$args" || fail 'normal mode passed telemetry setting'
    fi
}

capture_mode normal
normal="$TMP_ROOT/normal.remote"
grep -q '/tmp/cmd_to_run.sh' "$normal" || fail 'normal mode does not queue through Onion'
grep -q 'SYS_DIR=/mnt/SDCARD/.tmp_update' "$normal" || fail 'normal mode does not target Onion runtime directory'
grep -q 'RUNTIME_QUEUE=\$SYS_DIR/cmd_to_run.sh' "$normal" || fail 'normal mode does not verify runtime queue'
grep -q 'APP_DIR=/mnt/SDCARD/App/MiyooFin' "$normal" || fail 'normal mode does not use the MiyooFin app directory'
grep -q "exec '\$APP_DIR/launch.sh'" "$normal" || fail 'normal mode does not use packaged launcher'
grep -q 'kill -15' "$normal" || fail 'normal mode does not use the validated MainUI handoff signal'
! grep -Eq '(^|[[:space:]])(pkill|killall)([[:space:]]|$)|SIGKILL|kill -9|exec[[:space:]]+\./miyoofin' "$normal" || \
    fail 'normal mode contains unsafe direct process control'

capture_mode telemetry
telemetry="$TMP_ROOT/telemetry.remote"
grep -q 'LAUNCH_MODE=\${1-normal}' "$telemetry" || fail 'telemetry mode does not accept launch mode'
grep -q "TELEMETRY_LINE='export MIYOOFIN_TELEMETRY=1'" "$telemetry" || fail 'telemetry mode does not export telemetry'
grep -q 'APP_DIR=/mnt/SDCARD/App/MiyooFin' "$telemetry" || fail 'telemetry mode does not use the MiyooFin app directory'
grep -q "exec '\$APP_DIR/launch.sh'" "$telemetry" || fail 'telemetry mode does not use packaged launcher'

echo '[test] Onion-native remote launcher static contract OK'
