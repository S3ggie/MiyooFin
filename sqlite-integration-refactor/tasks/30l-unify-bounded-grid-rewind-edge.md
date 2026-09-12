# Task 30L — Unify the bounded grid rewind edge

**Phase:** B — corrective validation follow-up

## Objective

Use one bounded-grid edge rule for Movies, Shows, and Anime so reverse paging
works when the cursor reaches any card in the first retained row.

## Why

The current rewind trigger checks only index zero.  Holding Up from another
card in the first retained row—especially the rightmost card—does not move or
request the evicted earlier page, leaving the user inside the retained
window.

## Allowed Files

- `src/ui/screens/HomeScreenNavigation.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not change the bounded CatalogDb/LibraryQuery paging worker, schema,
  sorting, artwork, synchronization, offline authority, playback, or download
  behavior.
- Do not add a worker, connection, repository, UI-thread blocking work, or
  unbounded library materialization.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. Movies request the existing bounded rewind path when Up is pressed from any
   card in the first retained Movie row and earlier pages exist.
2. Normal Shows and Anime use the same shared top-row rule.
3. Up navigation within later rows remains unchanged.
4. A held Up button cannot create duplicate in-flight rewind requests.
5. Existing focus, selection, scroll, artwork, cancellation, supersession,
   sorting, and bounded-window behavior remain intact.

## Method

Add a focused source/behavior regression proving the shared top-row boundary
is used by all three grids.  Make the smallest UI navigation change by reusing
the existing generic `requestEarlierMediaPage` path.

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
- The shared edge rule requires changing paging ownership, worker/connection
  count, schema, or UI-thread behavior.
- Existing focus, sorting, artwork, cancellation, or supersession behavior
  cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): unify bounded grid rewind edge
```

Do not begin another hardware validation run until this task is committed.
