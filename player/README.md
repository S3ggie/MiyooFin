# miyoofin-player

Fork of FFmpeg 2.4.14's `ffplay` (the version Onion's stock ffplay is built from),
so decoding, A/V sync and SDL 1.2 behaviour match the stock player on the Miyoo.
It links the device's own libraries at run time (`/mnt/SDCARD/.tmp_update/lib/parasyte`,
`/mnt/SDCARD/miyoo/lib`); nothing from FFmpeg is packaged.

- `deps/build-deps.sh` builds LINK-ONLY copies of FFmpeg 2.4.14, SDL 1.2.15,
  freetype 2.10.4 and SDL_ttf 2.0.11 (pinned by SHA-256) into `output/player-deps`.
- `Makefile.player` cross-builds `output/build-arm/miyoofin-player`
  (`make -f Makefile.cross player` inside the toolchain container).
- `playback_runner.sh` prefers this binary and falls back to Onion's stock
  ffplay when it is missing, when `MIYOOFIN_PLAYER=stock`, or when it dies
  within 4 seconds of starting.

Licensing: `ffplay.c`/`cmdutils.c` are LGPL-2.1+ (see `COPYING.LGPLv2.1`); the device's
FFmpeg is built `--enable-gpl`, so treat the combined player as GPLv2+
(`COPYING.GPLv2`) and ship this directory's source with releases.

## miyoofin-audio

`audio.c` is the MiyooFin Music engine: a small audio-only program (no display, no network)
that plays local files gaplessly and is controlled over stdin/stdout. It links the same
device FFmpeg/SDL libraries and is built by `Makefile.player` together with the player; the same
licensing note applies. See `docs/music.md`.
