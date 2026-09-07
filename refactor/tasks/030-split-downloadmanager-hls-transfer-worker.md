# Task 030 — Split DownloadManager HLS transfer worker

## Status

NOT STARTED

## Depends On

Task 029

## Goal

Split DownloadManager HLS transfer worker. This task must be behavior-preserving.

## Allowed Files

- `src/download/DownloadManager.cpp`
- `src/download/DownloadManagerTransfer.cpp`
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

Create `DownloadManagerTransfer.cpp`.
Move worker/transfer/HLS segment retry/progress methods and only directly coupled file-local helpers.
This is a mechanical move: do not alter curl options, retry counts, delays, response handling, LAN/public fallback, partial-file replacement, cancellation, or completion validation.

## Behavior That Must Not Change

- HLS segment attempts remain identical.
- Cancellation never becomes public-route retry.
- Failed `.part` bodies are replaced before retry.
- Playback still interrupts downloading cleanly.
- TLS verification remains enabled.

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
git add src/download/DownloadManager.cpp src/download/DownloadManagerTransfer.cpp src/download/DownloadManager.hpp Makefile
git commit -m "refactor: split download transfer worker"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
