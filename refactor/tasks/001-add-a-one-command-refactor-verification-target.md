# Task 001 — Add a one-command refactor verification target

## Status

NOT STARTED

## Depends On

Task 000

## Goal

Add a one-command refactor verification target. This task must be behavior-preserving.

## Allowed Files

- `Makefile`
- `tools/refactor-check.sh`

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

Create `tools/refactor-check.sh` using POSIX `sh` with `set -eu`.
It must change to the repository root, run `make test`, then run `git diff --check`.
Add a `refactor-check` Makefile target that invokes `sh tools/refactor-check.sh`.
Do not change the existing `test` target.

## Behavior That Must Not Change

- `make test` behavior stays unchanged.
- No build flags or source lists change.

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
git add Makefile tools/refactor-check.sh
git commit -m "build: add refactor verification target"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
