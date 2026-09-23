#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
LAUNCHER="$ROOT/tools/desktop-run.sh"
RUNNER="$ROOT/distributions/onionos/playback_runner.sh"
MAKEFILE="$ROOT/Makefile"
DESKTOP_MAKEFILE="$ROOT/Makefile.desktop"
UI_RUNNER="$ROOT/tools/ui-script/run.sh"

fail() { echo "desktop runtime test failed: $*" >&2; exit 1; }

[ -x "$LAUNCHER" ] || fail 'desktop launcher is not executable'
[ -f "$DESKTOP_MAKEFILE" ] || fail 'desktop makefile is missing'
grep -q '^desktop-run:' "$DESKTOP_MAKEFILE" || fail 'desktop makefile has no desktop-run target'
grep -Fq '.DEFAULT_GOAL := all' "$MAKEFILE" || fail 'host build has no explicit default goal'
grep -Fq 'DEPS        := $(OBJS:.o=.d)' "$MAKEFILE" || fail 'host build has no generated header dependencies'
grep -Fq '$(CXX) $(CXXFLAGS) -MMD -MP' "$MAKEFILE" || fail 'host compile rule does not emit header dependencies'
grep -Fq -- '-include $(DEPS)' "$MAKEFILE" || fail 'host build does not include generated header dependencies'
grep -q 'MIYOOFIN_DESKTOP_INPUT=1' "$LAUNCHER" || fail 'launcher does not enable desktop input'
grep -Fq 'MIYOOFIN_UI_DIAGNOSTICS="$OUT/ui-stall.log"' "$UI_RUNNER" \
    || fail 'UI script runner does not route diagnostics per scenario'
grep -q 'MIYOOFIN_DESKTOP_WINDOW=1' "$LAUNCHER" || fail 'launcher does not enable native-size window mode'
grep -q 'MIYOOFIN_PLAYBACK_MODE=desktop' "$LAUNCHER" || fail 'launcher does not select desktop playback mode'
# Retain only the genuine wiring guard: the runner must keep an Onion default
# mode.  That branch is not host-executable (it cd's into the device tree and
# preloads the ARM audio bridge), so it has no behavioural seam.  Desktop case
# dispatch, driver clearing, and the configurable FFplay binary are asserted by
# executing the fixture below instead of by scanning the runner's source text.
grep -q 'PLAYBACK_MODE=.*onion' "$RUNNER" || fail 'playback runner has no mode selection'

# Execute the desktop branch with harmless helpers.  It must clear inherited
# Onion driver names before starting the host FFplay replacement.
TMP_ROOT=$(mktemp -d /tmp/miyoofin-desktop-test.XXXXXX)
trap 'rm -rf "$TMP_ROOT"' EXIT
cp "$RUNNER" "$TMP_ROOT/playback_runner.sh"
printf '%s\n' 'item_id=item' 'item_type=movie' 'resume_ticks=0' 'source_mode=jellyfin' > "$TMP_ROOT/playback-request.txt"
printf '%s\n' 'server_url=http://127.0.0.1:8096' 'access_token=test-token' > "$TMP_ROOT/session.txt"
cat > "$TMP_ROOT/miyoofin-https-bridge" <<'EOF'
#!/bin/sh
sleep 4
EOF
cat > "$TMP_ROOT/fake-ffplay" <<'EOF'
#!/bin/sh
printf 'video=%s\naudio=%s\n' "${SDL_VIDEODRIVER-unset}" "${SDL_AUDIODRIVER-unset}" > "$(dirname "$0")/ffplay-env.txt"
exit 0
EOF
chmod +x "$TMP_ROOT/miyoofin-https-bridge" "$TMP_ROOT/fake-ffplay"
(cd "$TMP_ROOT" && SDL_VIDEODRIVER=mmiyoo SDL_AUDIODRIVER=mmiyoo \
    MIYOOFIN_PLAYBACK_MODE=desktop MIYOOFIN_FFPLAY_BIN="$TMP_ROOT/fake-ffplay" \
    sh ./playback_runner.sh) >/dev/null 2>&1 || fail 'desktop playback branch did not exit cleanly'
[ -s "$TMP_ROOT/ffplay-env.txt" ] \
    || fail 'desktop branch did not invoke the configured FFplay binary'
grep -q '^video=unset$' "$TMP_ROOT/ffplay-env.txt" || fail 'desktop branch leaked video driver'
grep -q '^audio=unset$' "$TMP_ROOT/ffplay-env.txt" || fail 'desktop branch leaked audio driver'

echo '[test] desktop runtime launcher/playback contract OK'
