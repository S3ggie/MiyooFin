# Task 012 — Split HomeScreen lightweight refresh workers

## Status

NOT STARTED

## Depends On

Task 011

## Goal

Split HomeScreen lightweight refresh workers. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenRefresh.cpp`
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

Create `HomeScreenRefresh.cpp`.
Move:
- resume/Continue-Watching refresh worker methods
- Downloads snapshot/journal refresh worker methods
Keep their thread/atomic fields in HomeScreen.hpp for now.
Do not merge the two workers and do not change polling intervals/delays.

## Behavior That Must Not Change

- Resume refresh behavior remains identical.
- Downloads snapshots remain worker-published.
- Playback-journal reads stay off the render/input path.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenRefresh.cpp Makefile
git commit -m "refactor: split home refresh workers"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
