# Task 30A — Fix season refresh hierarchy contract

**Phase:** B — corrective validation follow-up

## Objective

Make `LibrarySync::refreshSeasons()` submit a valid partial hierarchy shape to
the existing CatalogDb worker contract without weakening hierarchy validation.

## Why

Task 31 hardware validation exposed that season refresh sends returned seasons
with an empty episode map. CatalogDb correctly rejects that inconsistent shape.

## Allowed Files

- `src/library/LibrarySync.cpp`
- `tests/cases/test_catalog_migration.inc`
- This task file

## Forbidden Scope

- Do not weaken or remove CatalogDb hierarchy validation.
- Do not modify Task 31 evidence files.
- Do not change SeriesScreen, EpisodeBrowserScreen, DownloadManager, playback,
  schema, worker/connection count, or UI-thread ownership.

## Architecture invariants

- `LibrarySync` remains the Jellyfin → SQLite synchronization owner.
- `LibraryQuery` remains the cached hierarchy read owner.
- `CatalogDb` remains the sole SQLite executor on its existing worker and
  connection.
- Partial season refreshes remain incomplete and must not delete existing
  episodes or claim a complete hierarchy checkpoint.
- Cancellation, generation, and scope supersession behavior remain unchanged.

## Implementation requirements

1. Add a focused regression using a local Jellyfin HTTP fixture and a seeded
   hierarchy. The test must prove that `refreshSeasons()` succeeds with more
   than one returned season, persists the seasons, and preserves existing
   episode rows while the hierarchy remains incomplete.
2. Verify the regression fails because the current empty episode map is
   rejected with `episode map does not match returned seasons`.
3. Change `refreshSeasons()` to pass an empty episode vector keyed by every
   returned season ID when staging the incomplete hierarchy.
4. Preserve existing cancellation and error propagation.

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

- Correctness requires weakening CatalogDb validation or changing another
  production subsystem.
- The fix requires schema v4, another SQLite worker/connection, unbounded
  reads, or blocking work on the SDL/UI thread.
- A required file falls outside **Allowed Files**.
- Required validation fails for an unrelated reason.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(library): preserve valid season refresh hierarchy
```

Do not begin Task 30B until this task is committed.
