# Toolchain: Miyoo Mini Plus cross-compilation

## Current Approach: Debian Cross-Compiler + Miyoo SDL2

The original Miyoo Buildroot toolchain from steward-fu's releases
(`toolchain.tar.gz`) is no longer available (404 from GitHub).

The current Dockerfile.onionos uses a clean, reproducible approach:

- **Base image:** `debian:buster` (glibc 2.28, matching the Miyoo device)
- **Cross-compiler:** Debian Buster's `gcc-arm-linux-gnueabihf`
  - GCC 8 family; the corresponding Debian Buster GCC source used for
    redistributed GCC runtime components is documented as `gcc-8` `8.3.0-6`
    in `THIRD_PARTY_NOTICES.md`
  - **Prefix:** `arm-linux-gnueabihf-`
  - **Sysroot:** `/usr/arm-linux-gnueabihf`
- **SDL2:** Miyoo-patched version from XK9274/sdl2_miyoo
  - **Branch:** `vanilla`
  - **Commit:** `478ddde6a415d48b4c497ce3a2679c08afd23f40`
  - Built with minimal options (no X11, OpenGL, ALSA, etc.)
  - Installed to cross-compiler sysroot

### Build the Docker image:

```shell
docker build -f Dockerfile.onionos -t miyoofin-toolchain .
```

### Cross-compile the application:

```shell
make onionos
```

### Verify the ARM binary:

```shell
make verify-arm
```

### Stage a package for local development/testing:

```shell
make package
```

This creates the OnionOS app directory under `output/package/`.

### Build a redistributable public release ZIP:

```shell
sh tools/build-release.sh
```

The release wrapper uses the existing package staging process, then adds
`LICENSE` and `THIRD_PARTY_NOTICES.md` before creating
`output/release/MiyooFin.zip`. See `RELEASING.md` for the release checklist.

## Docker image contents

- Debian Buster GCC 8 cross-compiler for `arm-linux-gnueabihf`
- Miyoo-patched SDL2 (libSDL2-2.0.so.0.18.2)
- GCC 8 C++ standard/runtime libraries used by the target build
- Binutils, make, cmake, autoconf, libtool

## Compiler flags

- `-std=c++17 -Wall -Wextra -Wpedantic -g -O2`
- No `--sysroot` is used — the Debian cross-compiler is already
  configured with correct built-in search paths
- SDL2 include: `-I/usr/arm-linux-gnueabihf/include/SDL2`
- SDL2 link: `-L/usr/arm-linux-gnueabihf/lib -lSDL2`

## Bundled libraries in package

The OnionOS package (`make package`) automatically bundles:
- `libSDL2-2.0.so.0` (Miyoo-patched — not provided by OnionOS)
- `libstdc++.so.6` (GCC C++ runtime)
- `libgcc_s.so.1` (GCC runtime support)

These are placed in `lib/` beside the binary. `launch.sh` sets
`LD_LIBRARY_PATH` to include this directory. Their license and corresponding
source information is maintained in `THIRD_PARTY_NOTICES.md` and is included
in public release ZIPs made by `tools/build-release.sh`.

## Miyoo build-time shared libraries

The patched SDL2 build requires six shared libraries from the Miyoo Mini /
Mini Plus as **build-time inputs**. These are not part of MiyooFin's source
distribution and must be supplied by the developer:

| Library | Device path |
|---|---|
| `libEGL.so.1` | `/mnt/SDCARD/.tmp_update/lib/parasyte/` |
| `libGLESv2.so` | `/mnt/SDCARD/.tmp_update/lib/parasyte/` |
| `libmi_ao.so` | `/config/lib/` |
| `libmi_common.so` | `/config/lib/` |
| `libmi_gfx.so` | `/config/lib/` |
| `libmi_sys.so` | `/config/lib/` |

After importing, the files live in `vendor/miyoo/lib/` and are copied
into the Docker image at build time. They are **never** copied into the
release package and are **not** tracked in Git.

### First-time setup

A Miyoo Mini / Mini Plus running OnionOS and reachable over SSH is
required for the import step. Enable SSH in OnionOS, then run:

```shell
make import-miyoo-libs
```

When run interactively, the import script asks for the Miyoo's IP address or
hostname and the SSH username. OnionOS uses `onion` by default when SSH
authentication is enabled; if SSH authentication is disabled, use `root`.
This means contributors do not need a machine-specific SSH alias named
`miyoo`.

For automation or an already-configured SSH alias, set a full target with
`MIYOO_HOST`:

```shell
MIYOO_HOST=onion@192.168.1.50 make import-miyoo-libs
```

The import script also accepts an explicit target when called directly:

```shell
tools/import-miyoo-build-libs.sh onion@192.168.1.50
```

The script fetches all six libraries into a temporary directory, verifies
every SHA-256 hash, and only then installs them. A partial or corrupt
import is rejected and the existing libraries remain unchanged.

After that, `make onionos` and `make package` will work as usual. Use
`tools/build-release.sh` when producing a public binary ZIP.

## CI toolchain image (private GHCR package)

CI cannot import the device blobs: `vendor/miyoo/lib/` is gitignored and no
Miyoo device is reachable from a GitHub runner. That is fine, because the app
itself does not need the blobs — only the SDL2 build inside the Docker image
does. The device supplies `libmi_*` at runtime, our binary's NEEDED list
contains no `libmi_*`/`libEGL` entries, and a build with an empty
`/opt/miyoo/lib` produces a byte-identical binary. So instead of importing
blobs in CI, we publish the finished toolchain image (cross-compiler plus the
already-built Miyoo-patched SDL2) as a private package and pull it in CI.

### One-time auth setup

Publishing needs the `write:packages` scope, which a default `gh` login does
not have:

```shell
gh auth refresh -h github.com -s write:packages
```

### Publish / refresh the image

From a machine that already has the local `miyoofin-toolchain:latest` image
(built via `docker build -f Dockerfile.onionos -t miyoofin-toolchain .`):

```shell
sh tools/push-toolchain-image.sh
```

The script tags the local image as
`ghcr.io/<owner>/miyoofin-toolchain:latest` (owner defaults to the lowercased
owner of git remote `origin`; override with an argument or `$TOOLCHAIN_OWNER`)
and pushes it with the label
`org.opencontainers.image.source=https://github.com/<owner>/MiyooFin`. That
label is what links the package to this repository on GitHub, which in turn
is what allows the workflow's `GITHUB_TOKEN` (`packages: read`) to pull the
private image. Auth comes from `gh auth token` when available, otherwise from
a prior `docker login ghcr.io`. Re-running is safe (same content pushes the
same digest) and prints the pushed digest at the end. Preview with:

```shell
sh tools/push-toolchain-image.sh --dry-run
```

### What the CI job does

The `arm-verify` job in `.github/workflows/ci.yml` logs in to GHCR, pulls the
private image, tags it locally as `miyoofin-toolchain:latest` (the tag `make
verify-arm` expects), cross-builds with the same invocation as the Makefile's
`onionos` target minus the blob preconditions (`make -f Makefile.cross
PERF_TELEMETRY=1 RELEASE=1 all bridge reporter`), and runs `make verify-arm`
(ARM architecture check plus the GLIBC <= 2.28 check inside the container).
`make onionos` / `make package` are deliberately not used: they depend on
`check-miyoo-libs` and would fail where the blobs cannot exist.

Note: the image is ~946MB, so it is pulled fresh on every run of the job.

## Investigation History

1. **steward-fu toolchain** — URL `toolchain.tar.gz` returned 302
   redirect then 404. Repository has been repurposed for the Drastic
   NDS emulator.
2. **nfriedly/miyoo-toolchain** — Docker Hub image exists but compiler
   not found at expected paths.
3. **miyoocfw/toolchain** — Contains `arm-buildroot-linux-musleabi`
   (musl-based, soft-float) — wrong ABI for our target.
4. **techdevangelist/miyoomini-buildroot** — No cross-compiler found.
5. **Debian Buster cross-compiler packages** — working GCC 8-family,
   hard-float, glibc-based toolchain compatible with the target ABI.
