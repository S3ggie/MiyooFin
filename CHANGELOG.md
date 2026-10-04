# Changelog

## 0.3.1

### Fixed
- **Downloads survive storage trouble.** An unreadable or swapped SD card is reported as "can't
  read your saved downloads" (never shown as an empty library) and is picked up again automatically
  when it returns; downloads that reappear are merged back without a restart. Disk writes, erase,
  pause/resume and account switches no longer wait on the card or leak another account's state.
- Downloaded HLS segments are checked to be real, complete media; oversized playlists fail cleanly
  instead of retrying forever; credentials stay on their own server across redirects.
- **Updater:** refuses unsafe archive entries and special files, rolls back files it created, and
  checks durability errors.
- **Music:** state, history, queue and pending plays are kept per account; a delete/re-add race in
  music downloads is closed; covers are cached at their drawn size; keep-in-sync no longer acts on a
  partly loaded list.
- Sign-in hands over as soon as it succeeds (Quick Connect needed an extra button press);
  WebSocket handling is bounded and handles disconnects.

### Internal
- Large test groups and the music screen / library coordinator sources are split by responsibility
  (no behavior change).

## 0.3.0

### New
- **MiyooFin Music** — a second mode with its own purple look: artists, albums, songs, playlists,
  queue with shuffle/repeat, gapless playback, Now Playing, mini-player, offline downloads, plays
  reported to Jellyfin, "Continue listening". See `docs/music.md`.
- **Playlists** (music) — add to playlist, new playlist, remove from and delete playlists, save the
  queue as a playlist.
- **Album grid view** (music) and **screen-off listening** (START + SELECT locks the buttons).
- **Mark watched / unwatched** for movies, episodes, seasons and series; offline changes sync later.
- **Skip intro / credits** prompt in the player (Jellyfin media segments).
- **Default audio and subtitle language** in Settings; a menu to pick the audio language when
  downloading; DVD/PGS subtitles via server burn-in.
- **Quick Connect** sign-in, two named server addresses (Home network / Internet) that can be edited
  or removed without signing out, and a **Test connection** row.
- Player: B shows or hides the bar; MENU quits only on a plain tap so the brightness combo is safe.
- **Crash report** screen (Settings → Diagnostics).
- **Favorites** (music): heart a song from its menu or from Now Playing (Down), a "Favorite songs"
  list in Playlists, and a song menu on Now Playing (add to playlist, go to album or artist).
- **Lyrics** in Now Playing (Up), following the song when they are time-synced.
- **Keep in sync** for downloaded playlists: new songs are downloaded, removed ones deleted unless
  another download still uses them.
- **Clock:** 12/24-hour setting and a time zone that is found automatically from the network (or
  chosen by hand).
- **Redesigned screens:** Settings grouped into Library / Playback / Display / Connection / App /
  Account, music Home as cover rails, cleaner album pages, better Now Playing title fit.

### Fixed
- Long playlists and albums were cut off at 500 songs when playing, queueing or downloading.
- DELETE requests were sent as GET (unmarking watched, deleting playlists, removing from a playlist).
- Music history reported the wrong stopped song after a queue jump; clearing the cache could delete
  files that were playing or queued.
- Sync label, column and counter fixes on Shows; Home tab restored after turning offline mode off.
