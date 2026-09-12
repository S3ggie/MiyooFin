# Task 30J — Preserve active grid selection through refresh

**Phase:** B — corrective validation follow-up

## Objective

Keep the user's active Movies or Shows/Anime item selected when bounded page
updates or a refresh rebuilds the presentation vectors.

## Why

The current Shows refresh restores from a preview identity that is not updated
when navigation moves the cursor.  A page completion can therefore restore the
first previously previewed title.  Movies also need a durable selection
identity when a refresh temporarily replaces its rendered row.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenNavigation.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not change catalog paging, sorting, artwork ownership, synchronization,
  offline authority, playback, downloads, or SQLite schema.
- Do not remove bounded reads or add workers, connections, repositories, or
  UI-thread blocking work.
- Do not alter the Shows/Anime classification or focus policy.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. A Movies page refresh restores the currently selected movie by identity when
   it remains available, including when the rendered row was temporarily empty.
2. A Shows or Anime page refresh restores the item currently under the cursor,
   rather than an older preview item.
3. Selection and grid scroll remain clamped and valid when the selected item is
   no longer available.
4. Normal navigation, alphabet filters, bounded paging, artwork scheduling,
   cancellation, and supersession remain unchanged.

## Method

Add a focused source/behavior regression proving refreshes capture active grid
identity and preserve it for both Movies and Shows/Anime.  Make the smallest
UI-state change in the existing navigation helpers.

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
- The fix requires changing paging, schema, worker/connection count, or UI
  thread behavior.
- Existing focus, classification, sorting, artwork, or cancellation behavior
  cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve active selection through refresh
```

Do not begin another hardware validation run until this task is committed.
