# Refactor2 Task 8 — Modularize LibrarySync incremental synchronization

## Objective

Split the incremental-change, authoritative-reconciliation, and live-event method definitions out of `LibrarySync.cpp` while keeping `LibrarySync` the single synchronization owner.

## Preconditions

- Task 7 commit exists.
- Baseline: catalog and cache/offline groups pass.

## Allowed Files

- `src/library/LibrarySync.cpp`, `LibrarySync.hpp`
- New `src/library/LibrarySyncIncremental.cpp`
- New `src/library/LibrarySyncEvents.cpp`
- A private `src/library/LibrarySyncInternal.hpp` only when a current file-local helper must be shared
- `src/net/JellyfinLibraryEvents.hpp` only for include hygiene, not behavior
- Catalog, cache/offline, and API/session tests
- `Makefile`, `Makefile.cross`, `Makefile.desktop`

## Required result

- Move `catchUpChangedCatalog`, `applyLibraryChanges`, authoritative membership reconciliation, and their exclusive helpers into `LibrarySyncIncremental.cpp`.
- Move live-event start/take/status plumbing and exclusive helpers into `LibrarySyncEvents.cpp`.
- Keep top-level/full synchronization, season/episode refresh, cancellation ownership, constructor, and common state in `LibrarySync.cpp`.
- Preserve event batching, checkpoint writes, generation behavior, periodic reconciliation, cancellation, mutex ordering, futures, API request order, and errors exactly.
- No new thread, callback framework, or public interface.

## Verification

Run catalog, cache/offline, and API/session binaries, then:

```sh
make test -j2
make -j2
git diff --check
```

STOP on any required behavior or ownership change.

## Commit

```text
refactor(sync): split incremental synchronization units
```
