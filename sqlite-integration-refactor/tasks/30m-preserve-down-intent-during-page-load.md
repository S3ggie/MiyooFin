# Task 30M — Preserve Down intent during page loading

**Phase:** B — corrective validation follow-up

## Objective

Make Movies, Shows, and Anime carry a held Down navigation intent across a
bounded page load instead of clamping the cursor to the current last card.

## Why

The grid movers clamp Down at the current last row while the next bounded page
is in flight.  A held button therefore pins the selection to the last visible
card, commonly the bottom-right card, and the later page does not advance the
selection into its next row.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenNavigation.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not remove bounded/keyset paging or materialize the full library.
- Do not add a worker, SQLite connection, repository, or UI-thread blocking
  work.
- Do not change sorting, Anime classification, artwork, synchronization,
  offline authority, playback, or download behavior.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. Movies, normal Shows, and Anime use one shared bounded-page Down-intent
   mechanism.
2. At the last available grid row, Down requests the existing next page but
   does not force selection to that row's last card while the page is in
   flight.
3. When the page succeeds, one queued Down intent advances to the matching
   column in the newly available row; repeated input cannot create duplicate
   intents or duplicate requests.
4. If no more page exists, existing end-of-grid clamping remains unchanged.
5. Existing Up rewind, focus, selection, scroll, artwork, cancellation,
   supersession, sorting, and bounded-window behavior remain intact.

## Method

Add a focused regression proving all three grids use the shared Down intent
state and apply it after successful page publication.  Implement only the
smallest navigation/state change using the existing media-page worker path.

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
- The change requires unbounded reads, schema changes, another worker/
  connection, or UI-thread blocking work.
- Existing focus, sorting, artwork, cancellation, supersession, or bounded
  paging behavior cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve down intent during page loads
```

Do not begin another hardware validation run until this task is committed.
