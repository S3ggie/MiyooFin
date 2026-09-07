# Task 034 — Extract cache/download/playback tests into include files

## Status

NOT STARTED

## Depends On

Task 033

## Goal

Extract cache/download/playback tests into include files. This task must be behavior-preserving.

## Allowed Files

- `tests/test_main.cpp`
- `tests/cases/test_cache_offline.inc`
- `tests/cases/test_downloads.inc`
- `tests/cases/test_playback_ui.inc`

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

Move existing cache/offline projection tests, DownloadManager/Download UI tests, and playback/external-handoff/progress tests into the three named `.inc` files.
Include them from test_main.cpp.
Preserve helper visibility by keeping include order compatible with current dependencies.
Do not change assertions or production source.

## Behavior That Must Not Change

- The same test functions are called by the same main().
- No new separate test process is introduced.

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
git add tests/test_main.cpp tests/cases/test_cache_offline.inc tests/cases/test_downloads.inc tests/cases/test_playback_ui.inc
git commit -m "test: split cache download playback cases"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
