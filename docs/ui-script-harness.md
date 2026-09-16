# Headless scripted-input UI harness

Test-only tooling that drives the **host/desktop build** through real UI
flows with scripted key input and asserts on the rendered framebuffer —
no Miyoo hardware, no real Jellyfin server. Lives in `tools/ui-script/`;
screenshots and logs land in `output/ui-script/<name>/`.

## Approach: LD_PRELOAD shim, not xdotool

The app reads all input through `InputManager::poll()` draining
`SDL_PollEvent` (`src/input/InputManager.cpp`), so the shim
(`tools/ui-script/shim.cpp`, built to `output/ui-script/shim.so`)
interposes exactly that function and replays a scripted key timeline.
This was chosen over `xdotool` because it is deterministic (no window
focus, no keymap/X11 timing flakiness), needs no extra packages
(xdotool/xwd are not installed anywhere this runs), and works unchanged
under `xvfb-run` in CI. Screenshots bypass X11 capture entirely: the
app already saves its framebuffer BMP on a `/tmp` flag file, and a
small opt-in hook (`MIYOOFIN_SCREENSHOT_PATH` in `App::pollScreenshotRequest`)
redirects that BMP per shot. No ImageMagick, no `xwd`, no new Python
packages — everything is POSIX `sh`, `g++`, and stdlib `python3`.

## What it runs without a server

`tools/ui-script/stub_server.py` is a stdlib-only loopback stub that
serves canned JSON for the endpoints the scripts hit: token validation
(`GET /Users/<id>`), Views, library pages, Resume/Latest rails, and
`Shows/<id>/Seasons` + `Episodes`. The runner seeds an isolated runtime
dir (a fresh `mktemp` cwd) with a `session.txt` pointing at
`http://127.0.0.1:<ephemeral-port>`, so the app exercises its real
fetch/render path with zero network, zero credentials, and zero
interference with your real session or cache. Artwork endpoints 404,
which exercises the app's placeholder path.

## Script format

`tools/ui-script/scripts/<name>.txt`, one directive per line
(`#` comments and blanks ignored):

```
WAIT_LOG <substring> [timeout_ms]   poll app stdout until it appears (default 30s)
KEY <name> [hold_ms] [settle_ms]    press+release (defaults 80ms / 400ms)
SCREENSHOT <name>                   save framebuffer BMP to shots/<name>.bmp
SETTLE <ms>                         bounded wait for marker-less async workers
QUIT                                inject SDL_QUIT so the app exits 0
```

Key names drive the desktop mapping (`MIYOOFIN_DESKTOP_INPUT=1`):
`Up/Down/Left/Right` (WASD), `Confirm` (Enter), `Back` (Backspace),
`NextTab/PrevTab` (T/E, i.e. the R/L shoulder scancodes).

Prefer `WAIT_LOG` over `SETTLE`: every wait has a bounded timeout and a
clear `FAIL` line in `output/ui-script/<name>/result.txt`, and the
runner exits non-zero. `SETTLE` exists for exactly one case — the
SeriesScreen seasons worker publishes with no log marker — and the
screenshot assertion after it is the real verdict.

## Assertions

`tools/ui-script/assert_shots.py` parses the 32-bit BMPs with stdlib
`struct` and applies coarse checks (never pixel-exact goldens, which
would be brittle across themes/fonts):

- `rendered` — frame is not blank/single-colour (≥8 distinct colours,
  no colour covers >88%).
- `rails` — the list/grid band (y 150–400) is populated, not empty
  (catches the grey-poster / "no seasons" class of bug).
- `seasons` — the Series-screen list band (y 150–460) is populated.

Screen reachability is asserted on stdout markers (e.g.
`[SeriesScreen] enter series=Testville`), which the runner greps from
the captured app log.

## Scripts

- `smoke.txt` — boot to Home, assert `Library loaded` + rendered frame
  with populated rails (Continue Watching / Recently Added from stub).
- `series.txt` — Home → `NextTab`×2 → Shows → `Right` into the grid →
  `Confirm` opens "Testville" → assert seasons listed.
- `smoke-device.txt` / `series-device.txt` — device counterparts used
  only by `device-run.sh` (roomier bounds, settle before screenshots,
  real-server content; see "Device scripts and the rails marker").

## Run it

```sh
make ui-script-test            # launcher round-trip, then both scripts (screenshots to output/ui-script/)
sh tools/ui-script/run.sh smoke    # one script
MIYOOFIN_UI_TIMEOUT_S=60 sh tools/ui-script/run.sh series
```

Needs a display: under CI the runner auto-uses `xvfb-run -a` when
`DISPLAY` is unset; locally it uses your session (a 640×480 window
pops up briefly). Override with `XVFB_RUN="xvfb-run -a"` or
`XVFB_RUN=` explicitly. The watchdog (`MIYOOFIN_UI_TIMEOUT_S`,
default 120s) kills hung runs instead of blocking CI.

To add a script: drop `scripts/<name>.txt` ending in `QUIT`, add its
marker/shot/checks row to the `case` in `run.sh`, and extend
`ui-script-test` if it should run by default.

## Device variant (ARM, Miyoo Mini Plus over SSH)

`tools/ui-script/device-run.sh` drives the **real installed ARM binary**
on the device through the **real privileged Onion launch path**: the
same `shim.cpp` cross-compiled to `output/ui-script/shim-arm.so`, the
device script files (`scripts/<name>-device.txt`), and the same
`assert_shots.py` oracle. Screenshots go through the same
`/tmp/miyoofin-screenshot-request` flag + app framebuffer hook that
`tools/miyoo/miyoofin-screenshot.sh` uses (the shim sets a per-shot
`MIYOOFIN_SCREENSHOT_PATH` under the device scratch dir instead of the
fixed `screenshot.bmp`).

```sh
make -f tools/ui-script/Makefile.arm verify   # build ARM shim + readelf/nm evidence
sh tools/ui-script/device-run.sh --dry-run smoke   # print every host/remote action, do nothing
sh tools/ui-script/device-run.sh smoke             # run smoke against the device
MIYOOFIN_UI_TIMEOUT_S=300 sh tools/ui-script/device-run.sh series
```

### Why privileged: non-root launch cannot work

Launching the binary directly over SSH runs as the default non-root
SSH user (`$MIYOO_SSH_TARGET`, usually `onion`), and that path is a
proven dead end, not a supported mode: `/dev/mi/gfx` and `/dev/mi/sys`
are root-only and `/dev/urandom` is `0660 root:root` on stock OnionOS,
so the app logs `failed to open /dev/mi/... (Permission denied)`,
never renders, and exits about one second after `[App] Initialisation
complete`. There is no root SSH and no sudo on the device — the only
privileged path is Onion's own handoff, which is what this harness
uses. Production launches the app as root via
`/mnt/SDCARD/App/MiyooFin/launch.sh`; the harness temporarily borrows
that exact path and gives it back (next section).

### Privileged flow step by step

1. Build the ARM shim; check preconditions over SSH: `launch.sh`
   exists, is executable, and contains the `./miyoofin` line; **no**
   harness backup is already present (refuse rather than clobber — see
   below); no `miyoofin` is running; MainUI is the sole foreground UI
   (the Onion handoff requires it).
2. `mkdir -p` the `/tmp/miyoofin-ui-script` scratch dir (`scp` does not
   create parents) and push `shim-arm.so` + the device script.
3. Copy `launch.sh` to `launch.sh.uiscript-bak` **on the device** with a
   plain `cp` (no `-p`), then rewrite `launch.sh` strictly **in place**
   (`cat ... > launch.sh`: no `chmod`, no `mv`, no rename) to add, just
   before `./miyoofin`:
   `LD_PRELOAD=<scratch>/shim-arm.so`, the `MIYOOFIN_UI_*` env
   (`SCRIPT/LOG/SHOT_DIR/RESULT`, plus `MIYOOFIN_UI_DEVICE_KEYS=1`
   unless `--desktop-keys`), truncates the app log and verdict file,
   and redirects the app's stdout into that log
   (`./miyoofin >>"$MIYOOFIN_UI_LOG" 2>&1`). The rest of `launch.sh`
   is untouched. In-place writes preserve the root-owned 0777 inode, so
   no ownership/permission syscall can fail midway: `cp -p` and `chmod`
   on the launcher both fail with `Operation not permitted` for the
   unprivileged SSH user, and are never used (not for backup, inject, or
   restore).
4. Trigger `tools/miyoo/onion-remote-launch.sh` in the background: it
   queues the same `/tmp/cmd_to_run.sh` handoff MainUI uses, MainUI
   exits, and the Onion runtime launches the app **as root** — with its
   REAL session and real server. The host polls the robust
   `/proc/[0-9]*/comm` loop for `miyoofin` (BusyBox `pgrep -x` is
   broken on this device), bounded at 120s.
5. Poll the scratch `result.txt` every 3s for the shim's verdict
   (`script complete, quitting app` or `FAIL`), bounded by
   `MIYOOFIN_UI_TIMEOUT_S` (default 300s). The device scripts end with
   `QUIT`, so a clean run self-exits; otherwise
   `tools/miyoo/onion-remote-exit.sh` exits it gracefully and MainUI's
   return is verified.
6. Pull `result.txt`, the shots, and the app log into
   `output/ui-script/device-<name>/`, **restore `launch.sh` with
   verification, remove scratch**, then run the same grep +
   `assert_shots.py` oracle as `run.sh`.

There is deliberately no stub server and no reverse SSH tunnel on this
path: the app uses its real session against the real server, exactly as
production does, so the whole tunnel/healthz complexity is gone from
the device script. (The stub still serves the desktop harness
`tools/ui-script/run.sh`, which has no server of its own.) The device
harness therefore exercises the real server/session; what it still
cannot verify is listed under "What the device variant can and cannot
verify" below.

### Backup/restore guarantee

The device must never be left with an injected launcher. Enforcement:

- The backup lives on the device next to the original
  (`<app>/launch.sh.uiscript-bak`). If it is already present at entry,
  the run **refuses immediately** with the manual restore command
  (`cat '<app>/launch.sh.uiscript-bak' > '<app>/launch.sh' && rm
  '<app>/launch.sh.uiscript-bak'`) instead of clobbering it — a stale
  backup means a previous run failed to restore, and only the operator
  can judge the installed launcher.
- Before touching `launch.sh`, the run pulls a pristine copy and records
  its on-device checksum; then `NEEDS_RESTORE` is armed, so an `EXIT`
  trap (with `INT`/`TERM` converted to failure exits so the same trap
  fires) restores on **every** path: success, assertion failure,
  timeout, injection failure partway through, `INT`, `TERM`. The inject
  and restore code is one shared file
  (`tools/ui-script/launcher-surgery.sh`) piped to the device over SSH —
  the offline test runs its exact bytes locally.
- Restore is verified, not assumed: write the backup back **in place**,
  `cmp` byte-compare against the backup, match the pre-injection
  checksum, confirm `MIYOOFIN_UI_`/`LD_PRELOAD=` are absent, confirm the
  executable bit is kept, then remove the backup. Restore is retried with
  backoff across transient SSH failures. Any step failing exits non-zero
  with a `CRITICAL` line carrying the exact manual recovery command.
- Scratch (`/tmp/miyoofin-ui-script`) is removed on the same trap, and
  the background launch-helper client is reaped; every wait is bounded
  (app appearance 120s, verdict `MIYOOFIN_UI_TIMEOUT_S`, helper reap
  30s, per-SSH `ConnectTimeout` 10s). The harness never reboots or
  power-cycles the device.

### Failure modes

- Helper rejects the handoff (MainUI not resident, stale queue, app
  already running): the run fails before anything is injected, naming
  the helper's reason.
- App never appears / never reaches a verdict: fail loudly after the
  bound; the launcher is still restored and verified.
- Graceful exit fails: a non-root SSH user cannot signal the root-run
  app, so there is no TERM/KILL escalation on this path — the run
  warns, restores the launcher regardless, and the operator exits the
  app physically if it is still up.
- Restore unverifiable: `CRITICAL` non-zero exit carrying the exact
  manual recovery command; fix by writing the named backup back in
  place (`cat '<app>/launch.sh.uiscript-bak' > '<app>/launch.sh'`)
  before launching from Onion again.

### Scancode mapping

`KEY <name>` resolves through the desktop table by default and through
the device table when `MIYOOFIN_UI_DEVICE_KEYS=1` is set (the device
runner sets it unless `--desktop-keys` is given), so the same script
file runs in both variants unchanged. Either mode also accepts the
`Raw:<scancode>` escape (e.g. `KEY Raw:79`) for buttons with no name.
Device codes are the physical scancodes from `InputManager`
(`src/input/InputManager.cpp`):

| Name      | Desktop   | Device (scancode) |
| Up        | W         | 82 (d-pad) |
| Down      | S         | 81 (d-pad) |
| Left      | A         | 80 (d-pad) |
| Right     | D         | 79 (d-pad) |
| Confirm   | Enter     | 44 (A button) |
| Back      | Backspace | 224 (B button) |
| NextTab   | T         | 23 (R shoulder) |
| PrevTab   | E         | 8 (L shoulder) |

Reachable only via `Raw:`: START=40, MENU=41, R2=42, L2=43, X=225,
Y=226, SELECT=228 (e.g. `KEY Raw:40`).

### Device scripts and the rails marker

The desktop scripts (`smoke.txt`, `series.txt`) are unchanged. The
device uses its own `smoke-device.txt` / `series-device.txt`: same
shape and key names (the shim remaps to device scancodes under
`MIYOOFIN_UI_DEVICE_KEYS=1`), but roomier bounds and an explicit settle
before each screenshot.

The settle is load-bearing, not cosmetic: on hardware the smoke
script's `WAIT_LOG [HomeScreen] Library loaded` matched while the rail
fetches were still in flight, so the screenshot captured `No content`
and the script's `QUIT` aborted them (`Transport: Operation was aborted
by an application callback`). The device scripts therefore wait for
`Library loaded`, then `SETTLE` (8s smoke, 5s home + 4s seasons
series), then screenshot — and the `rails`/`seasons` screenshot
assertion is the real rails-populated verdict.

Why not `WAIT_LOG` the rails directly: the `startup stage=`
markers (`continue_watching_finished`, `recently_added_finished`) are
written with `uiDiagnostics().log()`, which goes to the diagnostics
file — not to stdout — so the shim's log poll (which watches the
stdout capture) can never match them, and the app is out of scope for
test-only changes. If a stdout rails-complete marker is ever added to
the app, prefer it over the settle here.

`series-device.txt` runs against the real server, so it cannot name the
stub's series: its `WAIT_LOG [SeriesScreen] enter series=` relies on
substring matching and accepts whichever real series the focused Shows
tile opens. It needs at least one visible show; an empty Shows grid
fails loudly at the `WAIT_LOG` instead of screenshotting nothing.

### Safety rules (unattended hardware)

- Never reboots or power-cycles the device — no such command exists in
  the harness.
- The only writes outside `/tmp/miyoofin-ui-script` are the launcher
  backup + injected copy inside the app dir, both covered by the
  verified restore above. The installed binary and `lib/` are used
  read-only; the real session/catalog are used as production uses them
  (that is the point of the exercise).
- Refuses to start while a `miyoofin` is already running, while MainUI
  is not the sole foreground UI, or while a harness backup is already
  present.
- MainUI is never stopped or started directly; the Onion helpers own
  the handoff. Sole-MainUI residency is verified after the run
  (warning on mismatch, never a restart).
- Cleanup runs on EXIT/INT/TERM/timeout: graceful exit via
  `onion-remote-exit.sh` when the app is still resident (no
  TERM/KILL escalation exists — a non-root SSH user cannot signal the
  root-run app), then verified launcher restore, then scratch removal,
  then the launch-helper client is reaped. No device left with an
  injected launcher or the app running.
- Every wait is bounded (app appearance 120s, verdict
  `MIYOOFIN_UI_TIMEOUT_S` default 300s, helper reap 30s, per-SSH
  `ConnectTimeout` 10s); the whole run terminates deterministically.

### What the device variant can and cannot verify

Can: app logic and flow on the real ARM binary through the SDL event
boundary — privileged root launch through the production Onion
handoff, boot to Home against the real server/session, navigation
wiring with real device scancodes, rails/seasons rendering via the
framebuffer hook, clean exit behaviour. This is the path that will
carry download, playback, offline-mode and OTA flows once scripts
exist for them.

Cannot: the physical button→driver path (input is injected at
`SDL_PollEvent`, below the driver but above the buttons), real
video/audio output (framebuffer BMPs only), genuine SD-card/WiFi
timing (real server over WiFi, scratch on `/tmp`), or ARM performance
characteristics.

### Hardware status

Privileged launch verified once by a manual experiment: the app ran as
root, the log showed `[uishim] loaded 3 steps ...`, `result.txt`
contained `PASS WAIT_LOG matched: [HomeScreen] Library loaded`, `PASS
captured .../home.bmp`, `PASS script complete, quitting app`, and a
real 640x480 framebuffer BMP showed the actual device UI. The earlier
non-root attempt is the documented dead end above (permission-denied
`/dev/mi`, exit ~1s after initialisation).

Still to verify live: the automated `device-run.sh` end to end
(backup → inject → handoff → verdict → exit → verified restore),
including the rails settle timing and the `series-device.txt`
content-dependent navigation.

## CI

`.github/workflows/ci.yml` job `ui-script`: installs the same host
deps plus `xvfb`, builds, runs `make ui-script-test`. No Miyoo, no
GHCR image, no network beyond loopback.

## What it catches / cannot verify

Catches: app fails to boot to Home, fetch/render regressions that
leave rails or season lists empty, navigation wiring breaks (wrong
screen after a key sequence — the `WAIT_LOG` timeout fails loudly),
blank-frame regressions.

Cannot verify: physical Miyoo buttons/inputs, real video/audio
playback output, OnionOS driver behaviour, ARM performance. It drives
app logic and flow on the host build only.
