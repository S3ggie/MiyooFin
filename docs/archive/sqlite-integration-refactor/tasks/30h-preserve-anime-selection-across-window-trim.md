# Task 30H — Preserve Anime selection across bounded window trim

**Phase:** B — corrective validation follow-up

## Objective

Keep Anime selection and viewport state coherent when the bounded shared
Shows/Anime catalog window trims leading records after an incremental page.

## Why

Home stores normal Shows and Anime in separate filtered vectors, but the
bounded paging state is one mixed catalog window.  Subtracting the number of
trimmed mixed records from both filtered selection indices applies a normal
Show offset to Anime and can move Anime back to the wrong item or the top of
the grid while additional pages load.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not change the bounded page size, keyset ordering, or catalog ownership.
- Do not change Anime classification, poster fetching, SQLite, synchronization
  ownership, playback, offline behavior, or transfer/download behavior.
- Do not disable incremental updates or reintroduce full-library materialization.
- Do not modify Task 31 evidence or begin Task 32.

## Required behavior

1. Trimming the shared mixed Shows/Anime window must not subtract its count
   directly from either filtered selection index.
2. Existing ID-based selection restoration and grid-scroll preservation must
   retain an Anime item that remains in the window.
3. If the selected item was trimmed, existing deterministic fallback and
   focus/clamping behavior remains valid.
4. Normal Shows browsing continues to use the same policy.

## Method

Add a focused source/behavior regression showing that shared-window eviction
does not apply a global offset to the separate Anime index.  Make the smallest
production correction by letting `refreshShowsFilter()` restore the selection
from the selected item identity and clamp only when necessary.

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
- Existing bounded paging, sorting, Anime classification, artwork behavior,
  or cancellation cannot be preserved.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve anime selection across window trim
```

Do not begin another hardware validation run until this task is committed.
