# Task 30B — Preserve Anime focus during page updates

**Phase:** B — corrective validation follow-up

## Objective

Prevent incremental Home media-page updates from forcing the user out of
Anime and back to the normal Shows grid.

## Why

Task 31 hardware validation showed Anime content arriving incrementally, then
the presentation returning focus to normal Shows because the filter rebuild
always selected Shows when both collections were nonempty.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenNavigation.cpp`
- `tests/cases/test_misc_regressions.inc`
- This task file

## Forbidden Scope

- Do not disable bounded/incremental media updates or delay all publishing.
- Do not change Anime classification, deterministic sorting, artwork fetching,
  SQLite, schema, sync ownership, playback, or offline authority.
- Do not modify Task 31 evidence files.

## Architecture invariants

- Home remains a presentation consumer of bounded LibraryQuery results.
- Normal Shows and Anime selection/focus state remains UI-owned.
- Incremental updates remain bounded and cancellable.
- No SQLite/network/blocking work moves to the SDL/UI thread.

## Implementation requirements

1. Add a focused regression covering both cases: Anime remains selected when
   both Shows and Anime remain available after a refresh, and normal Shows
   remains selected in the corresponding normal browsing case.
2. Preserve the existing fallback behavior when the previously focused
   collection becomes empty.
3. Make the smallest UI-state change in `refreshShowsFilter()` so rebuilding
   presentation data retains valid focus and selection.

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

- Correctness requires changing media paging, Anime classification, artwork,
  synchronization, or another production subsystem.
- The fix requires schema v4, another SQLite worker/connection, unbounded
  reads, or blocking work on the SDL/UI thread.
- A required file falls outside **Allowed Files**.
- Required validation fails for an unrelated reason.

## Commit boundary

This task is exactly one commit. Use:

```text
fix(home): preserve anime focus during updates
```

Do not begin Task 31 until this task is committed.
