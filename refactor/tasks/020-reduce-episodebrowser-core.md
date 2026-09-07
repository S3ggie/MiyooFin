# Task 020 — Reduce EpisodeBrowser core

## Status

NOT STARTED

## Depends On

Task 019

## Goal

Reduce EpisodeBrowser core. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `src/ui/screens/EpisodeBrowserScreen.hpp`
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

Leave lifecycle, core navigation/input, and cross-concern coordination in EpisodeBrowserScreen.cpp.
Remove stale includes only.
Do not redesign class state or worker ownership.
Do not alter the public helper functions already covered by tests.

## Behavior That Must Not Change

- All EpisodeBrowser tests remain unchanged/passing.
- Worker shutdown remains bounded/cancellable.

## Focused Validation

Run:

```sh
`make refactor-check && wc -c src/ui/screens/EpisodeBrowserScreen.cpp`
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
git add src/ui/screens/EpisodeBrowserScreen.cpp src/ui/screens/EpisodeBrowserScreen.hpp Makefile
git commit -m "refactor: reduce episode browser coordinator"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
