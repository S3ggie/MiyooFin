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

## Run it

```sh
make ui-script-test            # both scripts, screenshots to output/ui-script/
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
on the device with the same scripted timeline: the same `shim.cpp`
cross-compiled to `output/ui-script/shim-arm.so`, the same script files,
and the same `assert_shots.py` oracle. Screenshots go through the same
`/tmp/miyoofin-screenshot-request` flag + app framebuffer hook that
`tools/miyoo/miyoofin-screenshot.sh` uses (the shim sets a per-shot
`MIYOOFIN_SCREENSHOT_PATH` under the device scratch dir instead of the
fixed `screenshot.bmp`).

```sh
make -f tools/ui-script/Makefile.arm verify   # build ARM shim + readelf/nm evidence
sh tools/ui-script/device-run.sh --dry-run smoke   # print every SSH action, do nothing
sh tools/ui-script/device-run.sh smoke             # run smoke against the device
MIYOOFIN_UI_TIMEOUT_S=300 sh tools/ui-script/device-run.sh series
```

The device cannot reach the host's loopback stub, so the runner starts
`stub_server.py <portfile> 0.0.0.0` on the host (loopback-only remains
the default; the LAN bind is opt-in per invocation) and seeds the
device scratch session with `server_url=http://<host-LAN-IP>:<port>`.
The host IP is auto-detected via `ip route get`; override with
`MIYOOFIN_UI_HOST_IP` when detection fails (VPNs, multiple NICs).
SSH target/port/key come from `tools/miyoo/ssh-common.sh`
(`MIYOO_SSH_TARGET`, `MIYOO_SSH_PORT`, `MIYOO_SSH_KEY`).

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

### Safety rules (unattended hardware)

- Never reboots or power-cycles the device — no such command exists in
  the harness.
- Never modifies or deletes user data: everything the harness writes on
  the device lives under `/tmp/miyoofin-ui-script` (shim, script,
  isolated runtime cwd with the seeded `session.txt`, shots, logs).
  The app runs with cwd=scratch, so its cwd-relative state (session,
  cache, downloads, catalog) lands there, not in the real app dir. The
  installed binary and `lib/` are used read-only.
- Refuses to start while a `miyoofin` is already running.
- MainUI is never stopped or started; its residency is recorded at entry
  and verified unchanged at exit (warning on mismatch, never a restart).
- Cleanup runs on EXIT/INT/TERM/timeout: graceful exit via the SIGUSR1
  helper when present (`/tmp/miyoofin-graceful-exit`, the same helper
  `onion-remote-exit.sh` uses), else SIGTERM, else SIGKILL — each step
  bounded (15s) — then scratch removal. No orphaned `miyoofin`, no
  device left with the app running.
- Every wait is bounded (stub port 10s, app run `MIYOOFIN_UI_TIMEOUT_S`
  default 180s, per-SSH `ConnectTimeout` 10s); the whole run terminates
  deterministically.

### What the device variant can and cannot verify

Can: app logic and flow on the real ARM binary through the SDL event
boundary — boot to Home against the stub, navigation wiring with real
device scancodes, rails/seasons rendering via the framebuffer hook,
clean exit behaviour. This is the path that will carry download,
playback, offline-mode and OTA flows once scripts exist for them.

Cannot: the physical button→driver path (input is injected at
`SDL_PollEvent`, below the driver but above the buttons), real
video/audio output (framebuffer BMPs only), genuine SD-card/WiFi
timing (stub over LAN WiFi, scratch on `/tmp`), or ARM performance
characteristics. Direct-launch alongside a resident MainUI is assumed
but NOT yet verified on hardware (see below).

### Hardware status

NOT yet verified against real hardware. Once the device is online:

```sh
make -f tools/ui-script/Makefile.arm verify
sh tools/ui-script/device-run.sh smoke
```

Troubleshooting on first hardware contact: if the app refuses the
`LD_PRELOAD` shim (setuid/suid env scrubbing), if the mmiyoo video
driver conflicts with a resident MainUI (blank/corrupt framebuffer
grabs), or if the device cannot route to the host stub IP, the run
fails loudly with the step named — fix that layer before trusting any
screenshot verdict.

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
