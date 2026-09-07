# Task 014 — Reduce HomeScreen core to lifecycle/coordinator code

## Status

NOT STARTED

## Depends On

Task 013

## Goal

Reduce HomeScreen core to lifecycle/coordinator code. This task must be behavior-preserving.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreen.hpp`
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

Review the now-smaller HomeScreen.cpp.
Leave constructor/destructor, enter/leave, update, and only genuinely cross-concern coordinator code in this file.
Remove stale includes made unnecessary by prior splits.
Do not change method bodies for style.
Do not move member fields between objects.
Do not change thread startup/shutdown order.

## Behavior That Must Not Change

- Screen lifecycle remains identical.
- Deferred destruction behavior remains identical.
- Worker shutdown order remains identical.

## Focused Validation

Run:

```sh
`make refactor-check && wc -c src/ui/screens/HomeScreen.cpp`
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
git add src/ui/screens/HomeScreen.cpp src/ui/screens/HomeScreen.hpp Makefile
git commit -m "refactor: reduce home screen coordinator"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
