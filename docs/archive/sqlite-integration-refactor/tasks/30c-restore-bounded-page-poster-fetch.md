# Task 30C — Restore bounded page poster fetching

**Phase:** B — corrective validation follow-up

## Objective

Restore Home poster fetching for Shows, Anime, and Movies that arrive through
bounded `LibraryQuery` media pages.

## Why

The pre-refactor Home path queued poster jobs from the complete remote snapshot.
The SQLite/lazy-page path now receives media in bounded pages, but successful
page completion only rebuilds presentation and queues cache decode. Missing
poster bytes are therefore never sent to the existing background poster worker.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreenHierarchy.cpp`
- `src/ui/HomeArtworkPlan.hpp`
- `src/ui/HomeArtworkPlan.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not replace the existing poster worker, add a worker, or move network or
  filesystem work to the SDL/UI thread.
- Do not change bounded media paging, Anime classification, focus policy,
  SQLite, schema, synchronization ownership, playback, or offline authority.
- Do not modify Task 31 evidence files.

## Architecture invariants

- Successful bounded media pages enqueue only their bounded poster jobs.
- `ImageCache` remains the artwork-byte cache.
- Poster HTTP and cache probing remain on the existing background poster
  worker; the UI path may only construct and enqueue jobs.
- Existing poster deduplication, cancellation/lifetime behavior, and route
  selection remain intact.

## Implementation requirements

1. Add a focused regression proving a bounded page containing normal Shows,
   Anime, and Movies with valid Primary tags produces poster jobs, while items
   without valid artwork remain excluded.
2. Enqueue jobs from successful bounded media-page completion for both movie and
   show pages.
3. Keep cache checks and poster HTTP requests on the existing poster worker so
   page completion does not perform filesystem or network work on SDL/UI.
4. Preserve existing full-snapshot/resume and season poster behavior.

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
- The fix requires another worker/connection, schema v4, unbounded reads, or
  blocking SQLite/network/filesystem work on the SDL/UI thread.
- Existing poster, paging, classification, or offline semantics cannot be
  preserved.
- Required validation fails for an unrelated reason.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): fetch posters for bounded media pages
```

Do not modify Task 31 evidence or begin Task 32.
