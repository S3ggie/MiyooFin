# Task 029 — Split DownloadManager reconcile worker

## Status

NOT STARTED

## Depends On

Task 028

## Goal

Split DownloadManager reconcile worker. This task must be behavior-preserving.

## Allowed Files

- `src/download/DownloadManager.cpp`
- `src/download/DownloadManagerReconcileWorker.cpp`
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

Create `DownloadManagerReconcileWorker.cpp`.
Move `requestReconcile`, reconciler-thread behavior, and directly coupled manager methods/helpers.
Do not modify `DownloadReconcile.cpp` policy in this task.
Keep the current delayed/bounded reconcile behavior and generation/scope checks.

## Behavior That Must Not Change

- Missing server items still become LocalOnly when appropriate.
- UpdateAvailable detection remains identical.
- Transient/Unauthorized behavior remains identical.

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
git add src/download/DownloadManager.cpp src/download/DownloadManagerReconcileWorker.cpp src/download/DownloadManager.hpp Makefile
git commit -m "refactor: split download reconcile worker"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
