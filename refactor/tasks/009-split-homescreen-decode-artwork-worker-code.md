# Task 009 — Split HomeScreen decode/artwork worker code

## Status

NOT STARTED

## Depends On

Task 008

## Goal

Split HomeScreen decode/artwork worker code. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenArtwork.cpp`
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

Create `HomeScreenArtwork.cpp`.
Move the selected-artwork, row-artwork, decode queue/worker, decoded-result draining, and decode submission member definitions.
Keep the exact mutex/condition-variable/atomic ownership in HomeScreen.
Do not combine the decode worker with poster/hierarchy workers.
Do not change cache keying or scheduling policy.

## Behavior That Must Not Change

- Selected-artwork behavior remains identical.
- Visible Shows decode scheduling remains identical.
- Cancellation/queue semantics remain identical.
- SDL thread still never decodes JPEGs.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenArtwork.cpp Makefile
git commit -m "refactor: split home artwork worker"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
