# MiyooFin user guide

MiyooFin has two modes in one app: **video** (Movies and Shows) and **Music** (purple).
Settings → *MiyooFin Music* switches to music; music Settings → *Enter MiyooFin* switches back.
The app reopens in the mode you last used.

## Signing in

1. Type your Jellyfin address (for example `http://192.168.1.10:8096`) and press START.
2. Sign in with a username and password, or press **SELECT** for **Quick Connect**: the Miyoo shows a
   code; on a phone or computer already signed in to Jellyfin open Settings → Quick Connect and
   enter it.

Settings holds two addresses, **Home network** and **Internet**. Either can be edited (A) or
removed (leave it empty and press START) without signing out; at least one must stay. The
**Test connection** row checks both and shows how long each took.

## Video

| Screen | Buttons |
|---|---|
| Browsing | D-pad move, A open, B back, L/R change tab |
| Movie page | Play, Download, Mark watched |
| Episodes / Series | SELECT opens *Watched* options (season and series ask to confirm) |
| Player | A pause, Left/Right seek 10 s, Up/Down seek 60 s, B show or hide the bar, SELECT track menu (audio and subtitles), X next subtitle, Y next audio, START fit or fill |
| Skip prompt | A skips the intro or credits, B hides the prompt |

Downloads ask which audio language to keep. A downloaded copy always plays instead of streaming.
**Settings → Offline mode** hides everything you have not downloaded. **Default audio language**
and **Default subtitles** set the starting choice for every video (switching in the player updates
them).

## Music

Music Home shows your *Continue listening* card and rails of recently added albums and recently
played songs: Left/Right move along a rail, Up/Down change rail.

| Context | Buttons |
|---|---|
| Lists | D-pad move, A open / play, B back, Y options, X play/pause, START now playing, SELECT queue, L2/R2 switch Library section |
| Now Playing | A pause, Left/Right seek, L/R previous/next, X shuffle, Y repeat, SELECT queue |
| Queue | A jump, Y remove, X save the queue as a playlist |

Y on a song, album or playlist offers *Play next*, *Add to queue*, *Add to playlist...* and
*Download*. In a playlist, Y on a song also offers *Remove from this playlist*; Y on a playlist
offers *Delete playlist* (choose it twice). *Settings → Album view* switches Albums between a list
and a cover grid.

**Screen-off listening:** press START and SELECT together while music is loaded. The screen goes
dark and every button is ignored; press them together again to wake it.

## Clock

The header clock follows your time zone. *Settings → Time zone* is **Automatic** by default: the app
asks a public IP lookup service (ip-api.com, zone name only) once per start while online and
remembers the answer; you can also pick a zone by hand, or use the device's own setting.
*Settings → Clock format* switches between 24-hour (`21:05`) and 12-hour (`9:05p`).

## Updates and problems

*Settings → Updates* installs new versions in place. If the app ever crashes, *Settings →
Diagnostics* opens the crash report (X clears it); sending it with a bug report helps.
