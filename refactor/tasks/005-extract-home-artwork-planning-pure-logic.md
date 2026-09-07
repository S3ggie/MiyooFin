# Task 005 — Extract Home artwork planning pure logic

## Status

NOT STARTED

## Depends On

Task 004

## Goal

Extract Home artwork planning pure logic. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/HomeArtworkPlan.hpp`
- `src/ui/HomeArtworkPlan.cpp`
- `Makefile`
- `tests/test_main.cpp`

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

Create `HomeArtworkPlan.hpp/.cpp`.
Move the pure poster-job value type and pure job/key creation helpers used for row posters and season posters.
Keep worker threads, queues, decoding, ImageCache reads/writes, and HTTP in HomeScreen.
Keep forwarding wrappers temporarily if needed by existing tests.

## Behavior That Must Not Change

- Artwork identity keys remain identical.
- Movie/show/episode dimensions remain identical.
- Season poster dimensions remain identical.
- No new I/O occurs in the pure module.

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
git add src/ui/screens/HomeScreen.hpp src/ui/screens/HomeScreen.cpp src/ui/HomeArtworkPlan.hpp src/ui/HomeArtworkPlan.cpp Makefile tests/test_main.cpp
git commit -m "refactor: extract home artwork planning logic"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
