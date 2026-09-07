# Task 023 — Split JellyfinApi library endpoints

## Status

NOT STARTED

## Depends On

Task 022

## Goal

Split JellyfinApi library endpoints. This task must be behavior-preserving.

## Allowed Files

- `src/net/JellyfinApi.cpp`
- `src/net/JellyfinApiLibrary.cpp`
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

Create `JellyfinApiLibrary.cpp`.
Move views, library item paging, latest, Continue Watching, tab-building/fetching endpoint method definitions that are not season/episode hierarchy calls.
Keep URL query parameters/order semantics and pagination behavior unchanged.

## Behavior That Must Not Change

- Library pagination remains complete.
- Latest keeps GroupItems=false.
- Sort/filter fields remain unchanged.
- No route fallback semantics change.

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
git add src/net/JellyfinApi.cpp src/net/JellyfinApiLibrary.cpp Makefile
git commit -m "refactor: split jellyfin library api"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
