# Task 021 — Split JellyfinApi JSON parsing implementation

## Status

NOT STARTED

## Depends On

Task 020

## Goal

Split JellyfinApi JSON parsing implementation. This task must be behavior-preserving.

## Allowed Files

- `src/net/JellyfinApi.cpp`
- `src/net/JellyfinApiJson.cpp`
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

Create `JellyfinApiJson.cpp`.
Move only JellyfinApi method definitions and file-local helpers whose purpose is JSON token/string/number/bool/array parsing and MediaItem conversion.
Keep every public `JellyfinApi` signature exactly unchanged.
Do not adopt a new JSON library and do not change escaping/Unicode behavior.

## Behavior That Must Not Change

- Direct-array parsing remains identical.
- Unicode and surrogate-pair behavior remains identical.
- 64-bit tick parsing remains identical.
- Media type normalization remains identical.

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
git add src/net/JellyfinApi.cpp src/net/JellyfinApiJson.cpp Makefile
git commit -m "refactor: split jellyfin json parsing"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
