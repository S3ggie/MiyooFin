# Task 032 — Extract Home/UI foundation tests into an include file

## Status

NOT STARTED

## Depends On

Task 031

## Goal

Extract Home/UI foundation tests into an include file. This task must be behavior-preserving.

## Allowed Files

- `tests/test_main.cpp`
- `tests/cases/test_ui_foundation.inc`

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

Create `tests/cases/`.
Move a coherent group of existing UI-foundation test function definitions from `test_main.cpp` into `tests/cases/test_ui_foundation.inc`.
Use `#include "cases/test_ui_foundation.inc"` from `test_main.cpp` at the original logical location.
Keep one translation unit: do NOT create a new executable, new `main`, new CHECK macro, or new global failure counter.
Move test bodies without changing assertions.

## Behavior That Must Not Change

- Exact tests still run from the existing main.
- No test assertion semantics change.

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
git add tests/test_main.cpp tests/cases/test_ui_foundation.inc
git commit -m "test: split ui foundation cases"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
