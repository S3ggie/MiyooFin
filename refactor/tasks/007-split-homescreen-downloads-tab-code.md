# Task 007 — Split HomeScreen Downloads-tab code

## Status

NOT STARTED

## Depends On

Task 006

## Goal

Split HomeScreen Downloads-tab code. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenDownloads.cpp`
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

Create `HomeScreenDownloads.cpp`.
Move only Downloads-tab presentation/input/snapshot helper member definitions from `HomeScreen.cpp`, such as:
- Downloads action handling
- Downloads tab drawing
- synchronous manipulation of the already-copied DownloadSnapshot/hierarchy
Do NOT move the background download-refresh thread methods yet; those belong to a later task.
Do not change DownloadManager.

## Behavior That Must Not Change

- Downloads controls and hierarchy behavior remain identical.
- Bulk removal still targets local DownloadManager IDs only.
- No server-side deletion behavior is introduced.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenDownloads.cpp Makefile
git commit -m "refactor: split home downloads ui"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
