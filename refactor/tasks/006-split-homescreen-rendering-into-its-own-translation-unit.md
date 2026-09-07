# Task 006 — Split HomeScreen rendering into its own translation unit

## Status

NOT STARTED

## Depends On

Task 005

## Goal

Split HomeScreen rendering into its own translation unit. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenRender.cpp`
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

Create `HomeScreenRender.cpp`.
Move only `HomeScreen` member definitions whose sole responsibility is drawing/rendering presentation.
At minimum this includes `render` and the `draw*` member functions that do not own worker lifecycle.
Move function bodies without behavioral edits. Keep signatures unchanged.
Add the new source file to the host/test build source list exactly as required by the existing Makefile.

## Behavior That Must Not Change

- Pixel/layout behavior is unchanged.
- Rendering still performs no new network/filesystem work.
- No navigation or worker logic changes.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenRender.cpp Makefile
git commit -m "refactor: split home screen rendering"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
