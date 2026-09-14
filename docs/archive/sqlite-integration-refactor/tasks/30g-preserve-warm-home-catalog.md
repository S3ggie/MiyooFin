# Task 30G — Preserve warm Home catalog windows

**Phase:** B — corrective validation follow-up

## Objective

Keep the bounded warm SQLite movie/show windows visible while the background
Jellyfin refresh completes, and prevent the first network page from replacing
the warm result with an empty tab shell.

## Why

Warm startup currently publishes bounded CatalogDb rows, then the UI reset
clears those rows and the paging windows.  The network worker can also replace
the warm result with `buildTabs(..., {}, {})` before publication.  The result is
an empty Movies/Shows presentation until the user enters a tab and triggers a
new lazy page read.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/HomeTabs.hpp`
- `src/ui/HomeTabs.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not remove bounded/keyset paging or materialize the full library.
- Do not move SQLite, HTTP, filesystem, retry, or blocking worker-join work to
  the SDL/UI thread.
- Do not change CatalogDb, schema, hierarchy synchronization, artwork worker
  mechanics, playback, download behavior, or offline authority.
- Do not change deterministic sorting or Anime classification policy.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. Warm bounded Movies and Shows rows remain available after Home publication.
2. The warm result is not replaced by an empty first-page shell while network
   synchronization continues.
3. Warm show membership retains Anime classification when the cached page
   identifies an Anime library.
4. Completed network refresh updates both Home rails without erasing valid
   warm catalog rows.
5. Subsequent bounded LibraryQuery pages merge through the existing paging
   path and retain selection/scroll behavior.
6. Fresh/empty catalogs retain their existing first-authoritative-page loading
   semantics.

## Method

Add focused pure regressions for extracting bounded media windows from the
published warm tabs and source/behavior regressions for preserving that state.
Verify they fail because the current path resets/overwrites warm data, then
make the minimum Home synchronization and projection changes.

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
- The fix requires unbounded reads, another worker/connection, schema v4, or
  blocking SQLite/network/filesystem work on the SDL/UI thread.
- Existing fresh-catalog, cancellation, sorting, Anime, focus, artwork, or
  offline semantics cannot be preserved.
- Required validation fails for an unrelated reason.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve warm catalog windows
```

Do not modify Task 31 evidence or begin Task 32.
