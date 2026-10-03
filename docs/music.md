# MiyooFin Music

MiyooFin Music is a second **mode** of the same app, not a separate binary. It is entered from
Settings (video side: "MIYOOFIN MUSIC"; music side: "Enter MiyooFin") and the app reopens in
whichever mode was used last (`app-mode.txt`). The two modes share the login, server routes and
the app lifecycle; everything else is separate so a bug in one cannot reach the other.

## What it looks like

Black background, white text, a purple accent (`#9650FF`) and a magenta highlight (`#F046DC`)
used for gradients on focus markers and progress bars. Green still means success and red an
error. The palette is runtime-selectable: `design::usePalette(bool music)` in `src/ui/Design.hpp`
swaps the colour variables; only the app calls it, on the UI thread, when the mode changes.

Five tabs under an always-visible header (L/R switch tabs): **Home**, **Library**
(Artists / Albums / Songs, switched with L2/R2), **Playlists**, **Downloads**, **Settings**.
A mini-player strip sits above the footer on every screen while something plays; START opens the
full **Now Playing** view; SELECT opens the queue.

Every tab remembers its own cursor, scroll and drill-down stack (album/artist/playlist pages) in
`music-ui-state.txt`, so leaving a page and coming back (or restarting the app) lands on the same
row. Only ids and cursors are kept; rows reload from the on-disk cache.

### Controls

| Context | Buttons |
|---|---|
| Lists | D-pad move, A open / play, B back, Y options, X play/pause, START now playing, SELECT queue, L2/R2 switch Library section or page |
| A-Z lists | Left/Right filter by letter (everything, A..Z, #) |
| Now Playing | A pause, Left/Right seek 10 s, L/R/L2/R2 previous/next, X shuffle, Y repeat, SELECT queue, B back |
| Queue view | Up/Down, A jump to track, Y remove from the queue |
| MENU | quits the app on a plain tap only (MENU+volume is OnionOS's brightness combo) |

## Architecture

```
MusicScreen (UI thread)  ->  MusicPlayer (UI thread API)  ->  miyoofin-audio (child process)
      |                           |  fetch thread: resolve/download a track to a local file
      |                           |  report thread: Jellyfin playback reports, queue saves
      |-> MusicLibrary (2 threads: listings + covers)      MusicDownloads (1 worker thread)
```

* `src/music/` is the domain layer (no SDL): `MusicTypes`, `MusicParse` (Jellyfin JSON),
  `MusicApi` (URLs and HTTP), `MusicCache` (listings on disk), `MusicLibrary` (background
  listing/cover loading), `MusicQueue` (play order, shuffle, repeat, saved queue),
  `MusicPlayer` (engine supervisor), `MusicTracks` (where a track's file comes from),
  `MusicDownloads` (offline music), `PlaysJournal`, `MusicSettings`.
* `src/ui/screens/MusicScreen*.cpp` is the UI; `src/ui/MusicUiState.*` is the persisted page state.
* `src/app/AppMode.*` remembers the mode; `App::switchMode` swaps the root screen.
* The video library is never touched: music has its own small caches and does not use
  `CatalogDb`/`LibraryCoordinator`. In music mode the coordinator is created without Home's
  startup reservation, and a switch back to video recreates it with one.

### Audio engine

`miyoofin-audio` (`player/audio.c`) is a small C program built with the same link-only FFmpeg 2.4 /
SDL 1.2 copies as `miyoofin-player`. It plays **local files only** (no network or TLS code): the
app downloads each track first. Decoding runs in its own thread into a ring buffer; markers in the
stream make `started`/`ended` events fire when the audio is actually heard, and the next track is
decoded into the same ring so changes are gapless. Output is always 44.1 kHz stereo 16-bit.

The app talks to it over its stdin/stdout, one line per message (see the header of `audio.c`):
`load <seconds> <path>`, `next <path>`, `nonext`, `pause`, `resume`, `seek <s>`, `stop`, `quit`;
events `ready`, `started`, `pos`, `ended`, `idle`, `error`.

`MusicPlayer` spawns it lazily with `LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so` (the OSS
emulation ffplay uses) and without the launcher's `SDL_AUDIODRIVER`/`SDL_VIDEODRIVER`. It polls
the pipes without blocking, kills a hung engine (no events for 6 s while playing), respawns after
a crash and resumes where it was (three crashes in a minute stop playback), and always stops it
before video playback starts. The engine must run as root on the device, so it is only ever
started by the app, never over SSH.

### Where a track's audio comes from

In order: a finished **download**, a file in the **stream cache** (`music-cache/stream`, 64 MB,
oldest evicted), or a fresh fetch into that cache. A fetch requests
`/Audio/<id>/stream.mp3?audioCodec=mp3&audioBitRate=<kbps>` (128/192/320, default 192) and checks the
result looks like audio before it is used. Offline mode never touches the network.

### Downloads

`MusicDownloads` is deliberately separate from the video `DownloadManager`: tracks are small
single files with nothing to resume, and music must never be able to disturb video downloads.
Files live in `music-downloads/tracks/<id>.mp3` with `music-downloads/index.tsv` listing tracks
and the albums/playlists/tracks that own them. One worker downloads sequentially with three
attempts and backoff, refuses to fill the card (keeps 200 MB free), pauses while offline mode is
on, notices files deleted behind its back, and shares a track between collections (removing one
keeps files another still needs). Y on an album, playlist or track offers Download; the Downloads
tab shows progress and works with no network at all.

### Reporting

Start / progress (every 10 s) / stopped reports go to the Sessions API from the report thread.
A play that cannot be reported (offline, server down) but counts as played (half the track or four
minutes) is written to `music-plays.journal` and sent later with `POST /Users/<id>/PlayedItems/<id>
?DatePlayed=...`, so play counts and "recently played" stay right.

### Resume

The queue (play order, position, shuffle, repeat, seconds) is saved to `music-queue.txt`
every 5 s while it changes and when playback stops, and cleared when the queue finishes. On the next
run it comes back paused as a "Continue listening" row at the top of Home.

## Files written next to the app

`app-mode.txt`, `music-ui-state.txt`, `music-settings.txt`, `music-queue.txt`,
`music-plays.journal`, `music-engine.log`, `crash.log` (fatal-signal backtraces for the whole app),
and the directories `music-cache/` (`lists`, `covers`, `stream`) and `music-downloads/`.

## Testing

Host tests (all deterministic, no network): `test_music` (JSON parsing, URLs, cache, queue, saved
queue, track source, UI state, mode file, plays journal), `test_music_player` (the supervisor against
`tests/fixtures/fake-audio-engine.sh`: sequencing, controls, skipping bad files, crash recovery,
reaping, resume, queue saving) and `test_music_downloads`. The threaded groups are also in
`make test-tsan`. The audio engine itself can only be exercised on the device.

On the device, build with `tools/miyoo/mfctl.sh deploy all` (the engine must come from the Docker
toolchain: a host-built binary needs a newer glibc than the Miyoo has) and drive the UI with
`mfctl.sh press ...` / `look`.

## Screen-off listening

Press START and SELECT together (within half a second) while music is loaded: the backlight goes
dark and every button, including MENU, is ignored. Press START + SELECT together again to
unlock. The previous backlight value is saved in `screen-lock.txt`, so a crash while locked is
undone the next time the app starts.
