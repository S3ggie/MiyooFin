# Task 30F — Restore initial Home artwork scheduling

**Phase:** B — corrective validation follow-up

## Objective

Restore bounded initial artwork scheduling for Home's Continue Watching,
Recently Added, and first network library pages without replacing the existing
poster worker.

## Why

The current warm/lazy Home path publishes before the old full-snapshot poster
enumeration runs.  Home rails therefore contain media items without creating
poster jobs until a later page-read or resume-refresh path happens.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/HomeArtworkPlan.hpp`
- `src/ui/HomeArtworkPlan.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not replace or add a poster worker.
- Do not perform HTTP, filesystem, or cache work on the SDL/UI thread.
- Do not enqueue an unbounded full-library poster plan from the population
  worker; only Home rails and the first bounded page from each library may be
  scheduled during initial synchronization.
- Do not change CatalogDb, schema, hierarchy synchronization, paging limits,
  navigation/focus, playback, download behavior, or offline authority.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. A successful initial Continue Watching/Recently Added fetch creates
   deduplicated poster jobs using the existing HomePoster worker path.
2. The first bounded page for each supported library creates only that page's
   poster jobs; later pages continue to use the existing bounded page-read
   scheduling path.
3. Items without valid display artwork remain excluded.
4. Existing poster cache checks, route selection, retries, cancellation, and
   season/resume poster behavior remain intact.

## Method

Add focused planner/source regressions first, verify they fail because the
initial synchronization path does not schedule jobs, then make the minimum
planner and call-site changes.

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
- The fix requires another worker/connection, schema v4, an unbounded initial
  poster queue, or blocking SQLite/network/filesystem work on the SDL/UI
  thread.
- Existing poster, paging, cancellation, or offline semantics cannot be
  preserved.
- Required validation fails for an unrelated reason.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): restore initial artwork scheduling
```

Do not modify Task 31 evidence or begin Task 32.
