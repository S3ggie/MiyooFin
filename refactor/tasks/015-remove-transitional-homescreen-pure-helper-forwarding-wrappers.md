# Task 015 — Remove transitional HomeScreen pure-helper forwarding wrappers

## Status

NOT STARTED

## Depends On

Task 014

## Goal

Remove transitional HomeScreen pure-helper forwarding wrappers. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/HomeSyncState.hpp`
- `src/ui/HomeSettingsModel.hpp`
- `src/ui/HomeTabs.hpp`
- `src/ui/HomeArtworkPlan.hpp`
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

Switch tests and internal call sites from temporary `HomeScreen::` forwarding helpers to their owning pure modules where practical.
Remove only forwarding declarations/definitions that are no longer needed.
Do not remove any stateful HomeScreen API used by App/screens.
If a wrapper has non-test production callers outside Allowed Files, leave it and report it.

## Behavior That Must Not Change

- Pure helper results remain identical.
- No user-visible HomeScreen API used by App is removed.

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
git add src/ui/screens/HomeScreen.hpp src/ui/screens/HomeScreen.cpp src/ui/HomeSyncState.hpp src/ui/HomeSettingsModel.hpp src/ui/HomeTabs.hpp src/ui/HomeArtworkPlan.hpp tests/test_main.cpp
git commit -m "refactor: remove home helper forwarding wrappers"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
