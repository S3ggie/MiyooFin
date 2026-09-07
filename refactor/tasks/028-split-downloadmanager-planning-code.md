# Task 028 — Split DownloadManager planning code

## Status

NOT STARTED

## Depends On

Task 027

## Goal

Split DownloadManager planning code. This task must be behavior-preserving.

## Allowed Files

- `src/download/DownloadManager.cpp`
- `src/download/DownloadManagerPlanning.cpp`
- `src/download/DownloadManager.hpp`
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

Create `DownloadManagerPlanning.cpp`.
Move plan creation/request/snapshot and planner-thread method definitions plus only file-local pure helpers required exclusively by planning.
Keep class signatures and synchronization behavior unchanged.
If a file-local helper is also used by transfer/core code, leave it in DownloadManager.cpp rather than duplicating or redesigning it.

## Behavior That Must Not Change

- Size estimates and safety reserve remain identical.
- Duplicate item de-duplication remains identical.
- Stale plan generations remain ignored.
- Planning remains off SDL thread.

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
git add src/download/DownloadManager.cpp src/download/DownloadManagerPlanning.cpp src/download/DownloadManager.hpp Makefile
git commit -m "refactor: split download planning"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
