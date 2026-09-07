# Task 033 — Extract API/session/artwork tests into include files

## Status

NOT STARTED

## Depends On

Task 032

## Goal

Extract API/session/artwork tests into include files. This task must be behavior-preserving.

## Allowed Files

- `tests/test_main.cpp`
- `tests/cases/test_api_session.inc`
- `tests/cases/test_artwork_episode.inc`

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

Move existing B3/B4 authentication/session/JSON tests into `test_api_session.inc`.
Move existing artwork/season/episode/prefetch pure tests into `test_artwork_episode.inc`.
Include both from test_main.cpp.
Keep all test function names and main() calls unchanged.
Do not duplicate helpers; because these are `.inc` files they remain in one translation unit.

## Behavior That Must Not Change

- All existing assertions and test order remain valid.
- No production files change.

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
git add tests/test_main.cpp tests/cases/test_api_session.inc tests/cases/test_artwork_episode.inc
git commit -m "test: split api and artwork cases"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
