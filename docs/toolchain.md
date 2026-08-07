# Toolchain: Miyoo Mini Plus cross-compilation

## Current Approach: Debian Cross-Compiler + Miyoo SDL2

The original Miyoo Buildroot toolchain from steward-fu's releases
(`toolchain.tar.gz`) is no longer available (404 from GitHub).

The current Dockerfile.onionos uses a clean, reproducible approach:

- **Base image:** `debian:bookworm`
- **Cross-compiler:** Debian's `gcc-arm-linux-gnueabihf` (GCC 12.2.0)
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

### Package for OnionOS:

```shell
make package
```

## Docker image contents

- GCC 12.2.0 cross-compiler for `arm-linux-gnueabihf`
- Miyoo-patched SDL2 (libSDL2-2.0.so.0.18.2)
- Cross-compiled C++ standard library
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
- `libstdc++.so.6` (cross-compiled C++ runtime)
- `libgcc_s.so.1` (GCC runtime support)

These are placed in `lib/` beside the binary. `launch.sh` sets
`LD_LIBRARY_PATH` to include this directory.

## Investigation History

1. **steward-fu toolchain** — URL `toolchain.tar.gz` returned 302
   redirect then 404. Repository has been repurposed for the Drastic
   NDS emulator.
2. **nfriedly/miyoo-toolchain** — Docker Hub image exists but compiler
   not found at expected paths.
3. **miyoocfw/toolchain** — Contains `arm-buildroot-linux-musleabi`
   (musl-based, soft-float) — wrong ABI for our target.
4. **techdevangelist/miyoomini-buildroot** — No cross-compiler found.
5. **Debian cross-compiler packages** — ✅ Working. GCC 12.2.0,
   hard-float, glibc-based, properly maintained.