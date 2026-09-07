# Task 013 — Split HomeScreen navigation and input code

## Status

NOT STARTED

## Depends On

Task 012

## Goal

Split HomeScreen navigation and input code. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreenNavigation.cpp`
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

Create `HomeScreenNavigation.cpp`.
Move top-level `handleAction` plus navigation-only helper member definitions:
- current tab/row/item lookup
- tab-name/index helpers
- navigation clamping
- movie/show focus/filter navigation helpers that are member-state based
Do not move lifecycle, update, or worker methods.
If a helper is shared with rendering and moving it would require redesign, leave it in HomeScreen.cpp.

## Behavior That Must Not Change

- D-pad behavior stays identical.
- Alphabet rail/grid transitions stay identical.
- Selected card/row/tab preservation stays identical.
- No I/O is added to action handling.

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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreenNavigation.cpp Makefile
git commit -m "refactor: split home navigation"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
