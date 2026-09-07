# Task 003 — Extract Home settings presentation model

## Status

NOT STARTED

## Depends On

Task 002

## Goal

Extract Home settings presentation model. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/HomeSettingsModel.hpp`
- `src/ui/HomeSettingsModel.cpp`
- `Makefile`
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

Create a small Home settings model module.
Move the pure settings address-row/action calculations out of `HomeScreen.cpp`.
The module owns the settings action enum/address-row value type and pure functions for:
- base row count
- address rows for a Session
- row count for a Session
- action for a row
Keep temporary `HomeScreen` forwarding methods if needed so existing callers/tests continue to compile.
Do not move rendering, mutable settings state, or request flags.

## Behavior That Must Not Change

- LAN/public address labels and ordering remain identical.
- All row indices and actions remain identical.
- No Session mutation is introduced.

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
git add src/ui/screens/HomeScreen.hpp src/ui/screens/HomeScreen.cpp src/ui/HomeSettingsModel.hpp src/ui/HomeSettingsModel.cpp Makefile tests/test_main.cpp
git commit -m "refactor: extract home settings model"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
