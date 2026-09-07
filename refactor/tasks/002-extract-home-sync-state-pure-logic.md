# Task 002 — Extract Home sync-state pure logic

## Status

NOT STARTED

## Depends On

Task 001

## Goal

Extract Home sync-state pure logic. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/HomeSyncState.hpp`
- `tests/test_main.cpp`

## Forbidden Scope

Every file not listed under **Allowed Files** is forbidden for this task.
Do not edit other task files. Do not perform opportunistic cleanup.

## Pre-change Checks

Run these before editing:

```sh
git status --short
make test
```

`git status --short` must be empty. If either check fails, STOP without editing.

## Exact Steps

Create `src/ui/HomeSyncState.hpp`.
Move these existing definitions from `HomeScreen.hpp` into it without semantic changes:
- `LibrarySyncSchedule`
- `ShowsSyncProgress`
- `librarySyncStatus`
Include the new header from `HomeScreen.hpp`.
Do not rename fields, constants, methods, parameters, return values, or timing constants.
Do not change the existing tests except include adjustments if compilation requires them.

## Behavior That Must Not Change

- 15-minute freshness remains identical.
- 60-second retry delay remains identical.
- Sync progress remains clamped/monotonic.
- Status strings remain byte-for-byte identical.

## Focused Validation

Run:

```sh
`make refactor-check`
```

## Required Final Validation

Run all of the following after the change:

```sh
make test
git diff --check
git status --short
git diff --stat
git diff
```

Read the full diff. Verify every changed path is listed under Allowed Files.

## Acceptance Criteria

- The stated extraction/change is complete.
- No unrelated behavior change is present.
- All required validation passes.
- Only Allowed Files changed.
- The worktree is clean after the commit.

## Commit

Commit exactly this task with:

```sh
git add src/ui/screens/HomeScreen.hpp src/ui/HomeSyncState.hpp tests/test_main.cpp
git commit -m "refactor: extract home sync state logic"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
