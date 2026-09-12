# Task 30D — Preserve Home grid position during page updates

**Phase:** B — corrective validation follow-up

## Objective

Keep the user's Movies, Shows, or Anime selection and viewport stable while a
bounded `LibraryQuery` page updates Home presentation data.

## Why

`refreshMovieFilter()` and `refreshShowsFilter()` currently rebuild their
filtered windows by resetting selection and scroll to zero.  Incremental page
completion therefore jumps the user back to the top while they browse.

## Allowed Files

- `src/ui/screens/HomeScreenNavigation.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not change bounded paging, sorting, Anime classification, poster
  fetching, SQLite, synchronization ownership, playback, or offline behavior.
- Do not add a worker or move network/filesystem work onto the SDL/UI thread.
- Do not change Home tab contents or transfer/download behavior.

## Required behavior

1. A successful incremental movie page preserves the selected movie when it
   remains in the filtered window and keeps its valid grid viewport.
2. A successful incremental Shows page preserves the selected normal Show or
   Anime item and the corresponding grid viewport when it remains available.
3. If the selected item is no longer available, retain deterministic bounded
   fallback/clamping behavior and keep the valid focus policy from Task 30B.
4. Existing artwork state remains stable for an item whose selection remains
   valid.

## Method

Add focused pure helper regressions for selection restoration and grid-scroll
clamping before changing the two refresh paths.  Make the smallest UI-state
change required; do not suppress or delay incremental publication.

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
- The fix requires unbounded reads, a new worker/connection, schema v4, or
  blocking SQLite/network/filesystem work on the SDL/UI thread.
- Existing focus, sorting, bounded population, or artwork semantics cannot be
  preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve grid position during page updates
```

Do not modify Task 31 evidence or begin Task 32.
