# Task 30K — Rewind bounded Home pages

**Phase:** B — corrective validation follow-up

## Objective

Keep bounded Home Movies and Shows/Anime navigation usable in both directions
after forward paging evicts the earliest page from the in-memory window.

## Why

Forward-only bounded windows currently discard the A-side after enough Shows
scrolling, leaving the user at the first retained title until the alphabet
rail is used.  Movie grid scroll also retains the pre-eviction row offset,
which can make the selected card alternate between the bottom-right and
top-right positions as pages arrive.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenNavigation.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not materialize the full library or remove bounded/keyset paging.
- Do not add SQLite workers, connections, repositories, or UI-thread blocking
  work.
- Do not change catalog sorting, Anime classification, artwork ownership,
  synchronization, offline authority, playback, or download behavior.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. When forward paging trims the front of a bounded Movies or Shows/Anime
   window, the state records that earlier pages are available.
2. Pressing Up at the first retained grid item starts a bounded rewind through
   the existing worker query path and replaces the window with the first page;
   it does not load the full library.
3. Rewound pages preserve cancellation/supersession and continue normal
   forward paging from the beginning.
4. Movie and Shows/Anime grid scroll offsets account for front-window eviction
   so a selected item does not jump between viewport edges.
5. Normal Shows, Anime, Movies, sorting, focus, artwork, and selection behavior
   remain intact.

## Method

Add a focused regression proving bounded page state supports earlier-page
rewind and eviction-aware grid positioning.  Implement the smallest state and
navigation changes using the existing `requestMediaPage` worker path.

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
- Rewind requires unbounded reads, schema changes, another worker/connection,
  or blocking work on the SDL/UI thread.
- Existing sorting, classification, focus, artwork, cancellation, or
  supersession behavior cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): rewind bounded pages without cursor jumps
```

Do not begin another hardware validation run until this task is committed.
