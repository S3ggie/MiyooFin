# Changelog

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

### Fixed
- DELETE requests were sent as GET (unmarking watched, deleting playlists, removing from a playlist).
- Music history reported the wrong stopped song after a queue jump; clearing the cache could delete
  files that were playing or queued.
- Sync label, column and counter fixes on Shows; Home tab restored after turning offline mode off.
