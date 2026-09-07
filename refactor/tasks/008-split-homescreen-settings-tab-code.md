# Task 008 — Split HomeScreen Settings-tab code

## Status

NOT STARTED

## Depends On

Task 007

## Goal

Split HomeScreen Settings-tab code. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenSettings.cpp`
- `Makefile`

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

Create `HomeScreenSettings.cpp`.
Move Settings-tab drawing and Settings-specific action handling/member helpers from HomeScreen.cpp.
Do not move generic top-level tab navigation.
Do not alter Session fields, request flags, confirmation timing, or labels.

## Behavior That Must Not Change

- Change-server/logout confirmations remain identical.
- Local/Public Address actions remain identical.
- Offline-mode behavior remains identical.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenSettings.cpp Makefile
git commit -m "refactor: split home settings ui"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
