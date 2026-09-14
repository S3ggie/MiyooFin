# Refactor2 Task 15 — Finish EpisodeBrowserScreen coordinator extraction

## Objective

Reduce `EpisodeBrowserScreen.cpp` to lifecycle/update coordination by extracting episode fetch/cache-refresh work that remains in the core after the existing render/artwork/playback/download splits.

## Preconditions

- Task 14 commit exists.
- Artwork/episode, cache/offline, playback, and download groups pass.

## Allowed Files

- All `src/ui/screens/EpisodeBrowser*.cpp` and `EpisodeBrowserScreen.hpp`
- New `src/ui/screens/EpisodeBrowserData.cpp`
- `src/library/LibrarySync.*` and `LibraryQuery.*` only for include cleanup; behavior/interface edits are forbidden
- Artwork/episode, cache/offline, playback, download, and misc tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move `fetchEpisodes` and closely related cache-read/Jellyfin-refresh/result-preparation definitions and exclusive helpers into `EpisodeBrowserData.cpp`.
- Keep constructor/destructor, enter/leave, update/poll coordination, and worker ownership in `EpisodeBrowserScreen.cpp`.
- Do not duplicate hierarchy refresh policy already owned by `LibrarySync`.
- Preserve cache-first episode publication, selection, sorting, played state, artwork prefetch, playback/download handoff, retry/error semantics, request order, cancellation/wakeup/join order, and screen-state lifetime.
- Do not merge or rewrite the existing render/artwork/playback/download units.

## Verification

Run artwork/episode, cache/offline, playback, download, and misc binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

STOP if moving fetch logic would require a new worker or altered ownership.

## Commit

```text
refactor(ui): isolate episode browser data work
```
