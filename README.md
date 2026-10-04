# MiyooFin

A native Jellyfin client for the **Miyoo Mini Plus running OnionOS**.
Browse movies, shows, and music; stream from your server or download media
for offline playback. Built with C++17, SDL2, libcurl, and SQLite.

[Download the latest release](https://github.com/S3ggie/MiyooFin/releases/latest) ·
[User guide](docs/user-guide.md) · [Contributing](CONTRIBUTING.md)

> **Unofficial third-party project:** MiyooFin is an independent client and is
> not affiliated with, endorsed by, or an official client of Jellyfin, Inc.,
> the Miyoo hardware manufacturer, or the OnionOS project. “Jellyfin”, “Miyoo”,
> and “OnionOS” are used only to identify software compatibility and the target
> platform. MiyooFin uses its own name and logo.

## Installation

**You need:** a Miyoo Mini Plus running [OnionOS](https://github.com/OnionUI/Onion),
a Jellyfin server, and a network connection for signing in, streaming, and downloading.
Completed downloads can be played offline.

1. Download **MiyooFin.zip** from the [latest release](https://github.com/S3ggie/MiyooFin/releases/latest).
   Use this asset for installation; the GitHub source-code archives are for developers.
2. Extract the ZIP and copy the `MiyooFin/` folder to your SD card:
   ```text
   SDCARD/App/MiyooFin/
   ```
3. Launch **Apps → MiyooFin** from the OnionOS menu.
4. Enter your Jellyfin server address and sign in with your username and password
   or Quick Connect. See the [sign-in guide](docs/user-guide.md#signing-in).

The release package includes the required application libraries. You do not need
to build the app or import device libraries to install a release.

### Updating

From version 0.2.0 onward, **Settings → Updates** installs new versions in place.
For older beta builds, download the latest ZIP and copy its `MiyooFin/` folder
over your existing installation. Keep your existing settings and download files;
a folder merge preserves them. Back up the app folder before a manual update.

## Features

- **Library browsing** — Movies and Shows (including separate library views such
  as Anime), drill-down from series → seasons → episodes, and an alphabet rail
  for jumping through large libraries.
- **Home rails** — Continue Watching and Recently Added, with resume positions
  from your server.
- **Downloads** — server-side transcoded HLS (H.264/AAC) for a movie, episode,
  season or whole series. Segmented and resumable, recoverable after a crash or
  reboot, with pause / resume / retry / delete.
- **Playback** — local-first: plays the downloaded copy when there is one, and
  otherwise streams. Progress is reported back to Jellyfin, so Continue Watching
  stays in sync.
- **Offline mode** — switch it on and the UI lists only what you have downloaded
  (the Home tab disappears); browsing and playback keep working with no network.
- **MiyooFin Music** — a second mode (Settings → MiyooFin Music) with its own
  purple look: browse artists, albums, songs and playlists, a play queue with
  shuffle/repeat, gapless playback in a separate audio engine while you keep
  browsing, album-art Now Playing, offline album/playlist downloads, and plays
  reported back to Jellyfin. See [docs/music.md](docs/music.md).
- **Subtitles and languages** — text and DVD/PGS subtitle tracks, a track menu in
  the player (SELECT), and default audio / subtitle languages in Settings.
- **Skip intro / credits** — the player offers a skip button when your Jellyfin
  server (10.10+) has marked the segment.
- **Mark watched / unwatched** — movies, episodes, seasons and series (with a
  confirmation for seasons and series); changes made offline sync later.
- **Playlists** — in Music: add to a playlist, create, remove from and delete
  playlists, or save the play queue as a playlist.
- **Favorites** — save favorite songs in Music and access them from Playlists.
- **Lyrics** — Up on Now Playing shows the song's lyrics, following along when they are time-synced.
- **Screen-off listening** — START + SELECT together darken the screen and lock
  the buttons while music plays; press them together again to unlock.
- **Signing in** — a username and password, or **Quick Connect** (approve the
  Miyoo from another Jellyfin client). Two server addresses (Home network and
  Internet) with a **Test connection** row in Settings.
- **Over-the-air updates** — check for and install new versions from the Settings
  screen (0.2.0 and later), with SHA-256 verification and rollback on failure.

Release asset sizes are listed on the [Releases page](https://github.com/S3ggie/MiyooFin/releases/latest).

## Screenshots

All screenshots are rendered from made-up libraries (the UI test server and a fake music
library), so they show no real server, account, playlists or cover art.

### Video

| Home | Movies |
|---|---|
| ![Home rails](docs/screenshots/video-home.png) | ![Movies grid with alphabet rail](docs/screenshots/video-movies.png) |

| Shows | Seasons |
|---|---|
| ![Shows grid](docs/screenshots/video-shows.png) | ![Season list](docs/screenshots/video-seasons.png) |

| Episodes | Movie page |
|---|---|
| ![Episode list with watched marks](docs/screenshots/video-episodes.png) | ![Movie page with Play, Download and Watched](docs/screenshots/video-movie.png) |

| Settings | |
|---|---|
| ![Settings, grouped into Library, Playback, Display, Connection, App and Account](docs/screenshots/video-settings.png) | |

### Music

| Home | Albums (grid) |
|---|---|
| ![Music Home: continue listening and cover rails](docs/screenshots/music-home.png) | ![Album grid](docs/screenshots/music-albums.png) |

| Album page | Now Playing |
|---|---|
| ![An album's tracks](docs/screenshots/music-album.png) | ![Now Playing with the mini-player controls](docs/screenshots/music-nowplaying.png) |

| Lyrics | Queue |
|---|---|
| ![Synced lyrics, the current line in white](docs/screenshots/music-lyrics.png) | ![Play queue](docs/screenshots/music-queue.png) |

| Playlists | Settings |
|---|---|
| ![Playlists tab](docs/screenshots/music-playlists.png) | ![Music settings](docs/screenshots/music-settings.png) |

See the [user guide](docs/user-guide.md) for the buttons and settings.

<details>
<summary>Regenerate the screenshots</summary>

Run `sh tools/ui-script/run.sh tour` and `sh tools/ui-script/run.sh tour-detail`
for video, or `make shots-music` for music. See the
[UI harness guide](docs/ui-script-harness.md) for the video workflow.

</details>

## Known limitations

- **No text search** — you move through the library with the alphabet rail
  (video) or the A-Z filter (music).
- **No quality picker for video** — downloads use one fixed profile tuned for the
  device's 640x480 screen (music has 128 / 192 / 320 kbps).
- **Skip intro needs server support** — Jellyfin 10.10 or later with intro
  detection (the Intro Skipper plugin or built-in segments).
- **No collections, instant mix or sleep timer** yet. (Favorites, playlists and lyrics are in Music; video has no favorites yet.)

## Status

Active development. Published versions and their changes are available on the
[Releases page](https://github.com/S3ggie/MiyooFin/releases/latest) and in the
[changelog](CHANGELOG.md). See [Known limitations](#known-limitations) before
installing; host tests and cross-build checks do not establish on-device behavior
for every release.

## Building

### Host development (Linux)

You need Git, GCC/G++ with C++17 support, GNU Make, `pkg-config`, and the
SDL2 and libcurl development packages. SQLite is included in the source tree.
Docker and a Miyoo device are needed only for the cross-build described below.

On Ubuntu 24.04, install the host build and check dependencies with:

```shell
sudo apt-get update
sudo apt-get install -y build-essential git pkg-config libsdl2-dev \
  libcurl4-openssl-dev python3 xvfb xauth clang-format-18
```

Clone the project and build:

```shell
git clone https://github.com/S3ggie/MiyooFin.git
cd MiyooFin
make -j2
```

This produces `output/build/miyoofin`. For the desktop development launcher,
install FFmpeg (including `ffplay`) and run `make desktop-run`. The host build
is a development environment; on-device playback still needs Miyoo hardware.

### OnionOS cross-build

You need Docker and a Miyoo Mini Plus running OnionOS, reachable over SSH
for a one-time import of six build-time libraries:

```shell
make import-miyoo-libs
```

The script asks for the device address and SSH username. Use `onion` when
OnionOS SSH authentication is enabled, or `root` when it is disabled.
For non-interactive use:

```shell
MIYOO_HOST=onion@192.168.1.50 make import-miyoo-libs
```

Then build, verify, and stage the OnionOS app folder:

```shell
make onionos JOBS=2
make verify-arm
make package JOBS=2
```

The staged folder is `output/package/MiyooFin/`. Imported device libraries
are build inputs and are not tracked or included in release packages.
See the [toolchain guide](docs/toolchain.md) for details.

For a public binary release, use `sh tools/build-release.sh`; it adds the
license and third-party notices and creates the ZIP, OTA tarball, and manifest.
Follow the [release checklist](RELEASING.md).

### Quality checks

During development, run focused checks for the files you change. Before a
normal code push, run the complete host check:

```shell
MIYOOFIN_JOBS=2 make ci-local
```

This includes formatting, the host build, tests and architecture boundaries,
ASan/UBSan, scripted UI flows, and whitespace checks. Use `make ci-local-full`
for substantial changes; it also cross-builds and verifies ARM binaries.
Concurrency changes additionally require `make test-tsan`.

On a shared Linux host, run heavy checks through the resource-limited wrapper
(for example `MIYOOFIN_JOBS=2 tools/bounded.sh make ci-local`). It requires a
working user systemd session. See [Contributing](CONTRIBUTING.md) and the
[code-quality guide](docs/code-quality.md) for validation and formatting rules.

To remove generated build artifacts, run `make clean`.

## Documentation and support

- [User guide](docs/user-guide.md): sign-in, controls, settings, and updates.
- [Music guide](docs/music.md): music controls, offline downloads, and audio architecture.
- [Architecture](docs/architecture.md): subsystem responsibilities and ownership.
- [Roadmap](ROADMAP.md): completed work and future directions.
- [Changelog](CHANGELOG.md): changes by release.

For help, search the [existing issues](https://github.com/S3ggie/MiyooFin/issues)
and open a [bug report](https://github.com/S3ggie/MiyooFin/issues/new?template=bug_report.yml)
with your versions and reproduction steps. Contributions are welcome;
see [CONTRIBUTING.md](CONTRIBUTING.md).

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
