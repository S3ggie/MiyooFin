# Task 035 — Reduce test_main.cpp to harness and test dispatch

## Status

NOT STARTED

## Depends On

Task 034

## Goal

Reduce test_main.cpp to harness and test dispatch. This task must be behavior-preserving.

## Allowed Files

- `tests/test_main.cpp`
- `tests/cases/test_misc_regressions.inc`

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

Move any remaining large standalone regression groups that are not needed as harness definitions into `test_misc_regressions.inc`.
Leave includes, shared CHECK macros/helpers that genuinely must precede all case includes, and `main()` in `test_main.cpp`.
Do not alter test order or names.

## Behavior That Must Not Change

- `test_main.cpp` remains the single existing test program.
- Every prior test still executes.

## Focused Validation

Run:

```sh
`make refactor-check && wc -c tests/test_main.cpp`
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
git add tests/test_main.cpp tests/cases/test_misc_regressions.inc
git commit -m "test: reduce main regression test source"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
