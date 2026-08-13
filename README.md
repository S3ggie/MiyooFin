# MiyooFin - A native Jellyfin client for the Miyoo Mini Plus (OnionOS)

Native Jellyfin media browser for the Miyoo Mini Plus running OnionOS.
Built with C++17, SDL2, libcurl, and json-c.

> **Unofficial third-party project:** MiyooFin is an independent client and is
> not affiliated with, endorsed by, or an official client of Jellyfin, Inc.,
> the Miyoo hardware manufacturer, or the OnionOS project. “Jellyfin”, “Miyoo”,
> and “OnionOS” are used only to identify software compatibility and the target
> platform. MiyooFin uses its own name and logo.

## Installation

**Requires:** Miyoo Mini Plus running [OnionOS](https://github.com/OnionUI/Onion).

1. Download the latest `MiyooFin.zip` release.
2. Extract it and copy the `MiyooFin/` folder to the SD card at:
   ```
   SDCARD/App/MiyooFin/
   ```
3. On your Miyoo, launch **Apps → MiyooFin** from the OnionOS menu.

No manual shared-library setup is required for a normal installation;
the prebuilt package is designed to run within the OnionOS environment.

## Status

Active development — browsing, playback, downloads, and offline mode
are functional.

## Building

### Prerequisites

- Docker (required for cross-compilation)
- GNU Make
- A Miyoo Mini Plus running OnionOS and reachable over SSH
  (needed once for the first-time library import step below)

### First-time setup

Enable SSH on the Miyoo, then fetch six external build-time libraries:

```shell
make import-miyoo-libs
```

The script prompts for the Miyoo IP address or hostname and SSH username.
OnionOS uses `onion` by default when SSH authentication is enabled; use
`root` when SSH authentication is disabled.

For non-interactive use, provide a full SSH target:

```shell
MIYOO_HOST=onion@192.168.1.50 make import-miyoo-libs
```

### Host (development)

```shell
make
```

Runs on the build machine. Produces `output/build/miyoofin`.

### OnionOS (cross-compilation via Docker)

```shell
make onionos
```

### OnionOS package

```shell
make package
```

Stages the ready-to-copy OnionOS App folder under `output/package/`.

For a public binary release, use `sh tools/build-release.sh` instead. That
wrapper adds the project license and third-party notices before creating the
redistributable ZIP. See [RELEASING.md](RELEASING.md).

### Verify ARM binary

```shell
make verify-arm
```

### Clean

```shell
make clean
```

For build-system details, see [docs/toolchain.md](docs/toolchain.md).

## Hardware

- **Device:** Miyoo Mini Plus
- **SoC:** SigmaStar SSD202D
- **CPU:** Dual-core ARM Cortex-A7 @ ~1.2 GHz
- **RAM:** 128 MB
- **Display:** 640x480
- **ABI:** ARMv7 hard-float (`arm-linux-gnueabihf`)
- **OS:** OnionOS (UI overhaul within the Miyoo firmware environment)

## License

GPL-3.0-only — see [LICENSE](LICENSE) for the full text.

Bundled-component licenses, notices, and source-availability information are
recorded in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
