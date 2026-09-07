# Task 011 — Split HomeScreen library sync worker

## Status

NOT STARTED

## Depends On

Task 010

## Goal

Split HomeScreen library sync worker. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
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

Create `HomeScreenSync.cpp`.
Move the full-library fetch/sync lifecycle members:
- start/request/finish fetch
- presentation projection application/restoration
- offline projection preparation/application
- sync status helper when directly tied to sync state
Do not move lightweight resume refresh or Downloads refresh yet.

## Behavior That Must Not Change

- Cached-first behavior remains identical.
- Transient network failure still leaves cached browsing available.
- Cache save/reconcile behavior stays identical.
- No fetch runs on the SDL thread.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenSync.cpp Makefile
git commit -m "refactor: split home library sync"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
