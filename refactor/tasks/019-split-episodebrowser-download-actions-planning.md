# Task 019 — Split EpisodeBrowser download actions/planning

## Status

NOT STARTED

## Depends On

Task 018

## Goal

Split EpisodeBrowser download actions/planning. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `src/ui/screens/EpisodeBrowserDownloads.cpp`
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

Create `EpisodeBrowserDownloads.cpp`.
Move episode/season download action handling, plan polling/confirmation, and DownloadManager interaction.
Do not modify DownloadManager itself.
Keep separate Episode and Season actions and existing plan-generation stale-result protection.

## Behavior That Must Not Change

- Episode and Season download controls remain separate.
- Plan acceptance/generation behavior stays identical.
- No storage I/O moves onto SDL thread.

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
git add src/ui/screens/EpisodeBrowserScreen.cpp src/ui/screens/EpisodeBrowserDownloads.cpp Makefile
git commit -m "refactor: split episode downloads"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
