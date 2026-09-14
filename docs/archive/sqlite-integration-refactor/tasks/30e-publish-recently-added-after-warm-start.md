# Task 30E — Publish Recently Added after warm start

**Phase:** B — corrective validation follow-up

## Objective

Publish the freshly fetched ephemeral Recently Added rail after Home has
already displayed warm SQLite catalog content.

## Why

Warm startup marks Home ready from bounded CatalogDb reads before the network
refresh completes.  The later network result currently cannot replace the
already-published tabs, so Continue Watching may appear through its dedicated
refresh while Recently Added is never inserted.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/HomeTabs.cpp`
- `src/ui/HomeTabs.hpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not change CatalogDb, schema, bounded paging, hierarchy synchronization,
  artwork, navigation, playback, or offline behavior.
- Do not block the SDL/UI thread on network or SQLite work.
- Do not persist Home rails into CatalogDb or legacy whole-file catalog data.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. Fresh startup continues to publish Recently Added with the initial Home
   result when the rail request succeeds.
2. Warm startup keeps its immediate bounded SQLite catalog publication, then
   inserts or replaces Recently Added when the completed background refresh
   supplies nonempty results.
3. A failed or empty Recently Added request does not erase valid Home content.
4. Continue Watching ordering and refresh behavior remain intact.

## Method

Add focused row-update regressions for insert, replacement, and empty/failure
preservation before wiring the warm completion path to the helper.

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
- The fix requires a new worker/connection, schema v4, unbounded reads, or
  blocking SQLite/network/filesystem work on the SDL/UI thread.
- Existing optional-rail failure isolation or compatibility behavior cannot be
  preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): publish recently added after warm start
```

Do not modify Task 31 evidence or begin Task 32.
