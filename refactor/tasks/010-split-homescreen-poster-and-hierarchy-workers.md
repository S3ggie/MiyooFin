# Task 010 — Split HomeScreen poster and hierarchy workers

## Status

NOT STARTED

## Depends On

Task 009

## Goal

Split HomeScreen poster and hierarchy workers. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenHierarchy.cpp`
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

Create `HomeScreenHierarchy.cpp`.
Move poster-sync queue/worker and hierarchy-cache scheduling/worker member definitions.
Move only helper methods directly coupled to those workers, including cached-season lookup if it belongs to this concern.
Do not change request ordering, generation checks, worker count, or cache reconciliation semantics.

## Behavior That Must Not Change

- Hierarchy discovery remains a single bounded worker.
- Generation cancellation stays identical.
- Poster work stays off SDL thread.
- OfflineCatalog write/reconcile behavior stays identical.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenHierarchy.cpp Makefile
git commit -m "refactor: split home hierarchy workers"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
