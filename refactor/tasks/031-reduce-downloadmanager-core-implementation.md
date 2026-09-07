# Task 031 — Reduce DownloadManager core implementation

## Status

NOT STARTED

## Depends On

Task 030

## Goal

Reduce DownloadManager core implementation. This task must be behavior-preserving.

## Allowed Files

- `src/download/DownloadManager.cpp`
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

Leave constructor/destructor/configure, queue state mutation, snapshot, persistence scheduling, and other cross-concern core methods in DownloadManager.cpp.
Remove stale includes only.
Do not redesign mutex ownership, condition variables, atomics, state enum, or persistent storage.

## Behavior That Must Not Change

- Queue/pause/resume/erase behavior remains identical.
- Manifest-first/index persistence remains identical.
- Snapshot accounting remains identical.

## Focused Validation

Run:

```sh
`make refactor-check && wc -c src/download/DownloadManager.cpp`
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
git add src/download/DownloadManager.cpp src/download/DownloadManager.hpp Makefile
git commit -m "refactor: reduce download manager core"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
