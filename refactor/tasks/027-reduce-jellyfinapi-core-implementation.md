# Task 027 — Reduce JellyfinApi core implementation

## Status

NOT STARTED

## Depends On

Task 026

## Goal

Reduce JellyfinApi core implementation. This task must be behavior-preserving.

## Allowed Files

- `src/net/JellyfinApi.cpp`
- `src/net/JellyfinApi.hpp`
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

Review JellyfinApi.cpp after the endpoint-family splits.
Leave only genuinely shared/core definitions.
Remove stale includes and duplicate file-local helpers only when proven unused.
Reorganize comments/declaration grouping in JellyfinApi.hpp without changing public signatures.
Do not rename methods.

## Behavior That Must Not Change

- All existing JellyfinApi call sites compile unchanged.
- No endpoint behavior changes.

## Focused Validation

Run:

```sh
`make refactor-check && wc -c src/net/JellyfinApi.cpp`
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
git add src/net/JellyfinApi.cpp src/net/JellyfinApi.hpp Makefile
git commit -m "refactor: reduce jellyfin api core"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
