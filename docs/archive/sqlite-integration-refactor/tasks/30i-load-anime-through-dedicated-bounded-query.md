# Task 30I — Load Anime through a dedicated bounded query

**Phase:** B — corrective validation follow-up

## Objective

Populate Home's Anime section from an Anime-specific bounded catalog query so
Anime titles do not depend on their position in the global Shows page.

## Why

Home currently reads one mixed, bounded Shows page and classifies only the
Anime items that happen to fall into that page.  On the physical catalog this
left two Anime titles visible while the remaining Anime memberships were
outside the first page.  The stable presentation showed the complete Anime
section independently of normal Shows ordering.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `src/library/LibraryQuery.hpp`
- `src/library/LibraryQuery.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not remove bounded/keyset paging or materialize the full library in
  normal runtime memory.
- Do not add a SQLite connection, worker, repository layer, or UI-thread
  SQLite/network/filesystem work.
- Do not change Anime classification semantics, sorting, artwork ownership,
  synchronization ownership, hierarchy behavior, playback, offline
  authority, or transfer/download behavior.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. `LibraryQuery` exposes a domain-level Anime page read.
2. CatalogDb executes the Anime filter on its existing worker/connection,
   selecting TV items with Anime view membership or the Anime genre while
   preserving deterministic keyset ordering and bounded limits.
3. Home keeps Anime paging independent from the mixed Shows page and merges
   only bounded results into the existing Shows/Anime presentation.
4. The first Anime query uses the existing bounded worker path and loads the
   current catalog's Anime titles without waiting for normal Shows ordering.
5. Existing focus, selection restoration, scroll preservation, cancellation,
   supersession, artwork fetching, and normal Shows behavior remain intact.

## Method

Add source and CatalogDb/LibraryQuery regressions first, verify the current
Home path has no dedicated Anime page and the fixture cannot query Anime
independently, then make the minimum query and Home state changes.

## Validation

Run from the repository root:

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
git diff --check
```

## STOP conditions

- Correctness requires changing files outside this Allowed Files list.
- The filter requires an unbounded read, schema v4, another worker/connection,
  or blocking work on the SDL/UI thread.
- Existing Anime classification, sorting, cancellation, artwork, or normal
  Shows behavior cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): load anime through dedicated bounded query
```

Do not begin another hardware validation run until this task is committed.
