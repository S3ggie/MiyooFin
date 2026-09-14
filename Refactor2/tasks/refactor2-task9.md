# Refactor2 Task 9 — Modularize Home synchronization projections

## Objective

Separate HomeScreen's offline projection, worker-side fetch preparation, and UI-thread result application so `HomeScreenSync.cpp` becomes coordination rather than a mixed projection/worker/update unit.

## Preconditions

- Task 8 commit exists.
- Baseline: UI foundation, cache/offline, and catalog test groups pass.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`, `HomeScreen.hpp`, `HomeScreen.cpp`
- New `src/ui/screens/HomeScreenOffline.cpp`
- New `src/ui/screens/HomeScreenSyncApply.cpp`
- Existing `src/ui/Home*` pure-model files or one new narrowly scoped pure-model pair
- `src/cache/OfflineLibraryProjection.*` only if moving already-existing projection logic
- UI foundation, cache/offline, catalog, and misc regression tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move `prepareOfflineProjection`/`applyOfflineProjection` and their exclusive helpers to `HomeScreenOffline.cpp`.
- Move UI-thread publication/result-application definitions and exclusive pure transition helpers to `HomeScreenSyncApply.cpp`.
- Keep worker launch, polling, cancellation requests, and lifecycle coordination in `HomeScreenSync.cpp`.
- Do not alter rail/tab contents, sorting, selection retention, pagination, empty/error states, refresh cadence, or cached-first publication.
- Do not perform network, database waits, cache scans, filesystem work, or blocking joins on the UI thread.
- Do not move Jellyfin-to-SQLite synchronization ownership back into HomeScreen.

## Verification

Run UI foundation, cache/offline, catalog, and misc binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

STOP if lifetime/cancellation ordering cannot remain identical.

## Commit

```text
refactor(home): isolate sync projections and publication
```
