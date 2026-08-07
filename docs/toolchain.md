# Toolchain investigation for Miyoo Mini Plus cross-compilation

## Source Repository

**XK9274/sdl2_miyoo** (fork of steward-fu/nds_miyoo)
- URL: https://github.com/XK9274/sdl2_miyoo
- Branches: main, vanilla, onion, moonlight, pico8, srb2, snow-s-mania

## Branch Assessment

### `main` (commit bf1aba2de04ed8c3401cf45919e0ae63e58d2cec)
- ⚠️ **Warning in README:** "The main branch of this fork is out of date."
- Contains NDS-specific logic.
- **Not recommended** for MiyooFin.

### `vanilla` (commit 478ddde6a415d48b4c497ce3a2679c08afd23f40)
- ✅ **Recommended by README:** "The vanilla branch has most of the NDS
  specific logic removed and can be used at runtime for various apps."
- **Pinned commit:** `478ddde6a415d48b4c497ce3a2679c08afd23f40`
- This is the branch to use for the MiyooFin toolchain.

### `onion` (commit 64744fc6546cbb11bec8fcb20461e4282246acf5)
- Potentially more specific to OnionOS, but not explicitly recommended.

## Compiler Prefix

From `Makefile` (main branch, same structure as vanilla):
```
CROSS=/opt/mmiyoo/bin/arm-linux-gnueabihf-
```

The compiler prefix is **`arm-linux-gnueabihf-`**:
- `arm-linux-gnueabihf-gcc`
- `arm-linux-gnueabihf-g++`
- `arm-linux-gnueabihf-ar`
- `arm-linux-gnueabihf-ld`

## Sysroot

From `Makefile` example build command:
```
-I/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot/usr/include/SDL2
```

The sysroot is at: **`/opt/mmiyoo/arm-buildroot-linux-gnueabihf/sysroot/`**

## Dockerfile

The Dockerfile on `main` branch:
```dockerfile
FROM debian:buster
RUN apt-get update
RUN apt-get install build-essential make cmake wget -y
RUN cd && wget https://github.com/steward-fu/nds_miyoo/releases/download/assets/toolchain.tar.gz
RUN cd && tar xvf toolchain.tar.gz
RUN cd && mv mmiyoo /opt
RUN cd && mv prebuilt /opt
RUN export PATH=/opt/mmiyoo/bin/:$PATH
```

The toolchain is downloaded from steward-fu's release assets:
`https://github.com/steward-fu/nds_miyoo/releases/download/assets/toolchain.tar.gz`

This is a prebuilt Buildroot toolchain targeting `arm-linux-gnueabihf`.

## Toolchain Acquisition Plan

### Option A: Use the Dockerfile from XK9274/sdl2_miyoo (vanilla branch)

1. **Command:** `docker build -f Dockerfile.onionos -t miyoofin-toolchain .`
2. **Source:** steward-fu/nds_miyoo releases (toolchain.tar.gz) +
   sdl2_miyoo/vanilla branch for SDL2 source
3. **Download size:** ~200 MB (toolchain) + ~20 MB (SDL2 source)
4. **Reason:** This is the established community toolchain specifically
   targeting the Miyoo Mini Plus. It matches the known CPU architecture
   (SigmaStar SSD202D, Cortex-A7, ARMv7 hard-float). The `vanilla` branch
   is explicitly recommended for application development.

### Option B: Build from OnionOS SDK sources

Requires finding the official OnionOS Buildroot SDK. Not yet located.

### Recommendation

Start with **Option A** (the sdl2_miyoo vanilla Dockerfile approach),
since it is the most well-documented path and avoids building a full
Buildroot toolchain from source (~10+ hours).

## Next Steps

1. Inspect the Dockerfile.onionos we will author
2. Confirm the `docker build` command, download size, and source
3. Request approval before downloading
4. After build, verify the toolchain with a test compile