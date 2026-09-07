# Task 036 — Update architecture documentation to match refactored layout

## Status

NOT STARTED

## Depends On

Task 035

## Goal

Update architecture documentation to match refactored layout. This task must be behavior-preserving.

## Allowed Files

- `docs/architecture.md`
- `refactor/STATUS.md`

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

Update `docs/architecture.md` to describe the actual post-refactor file/responsibility layout.
Document that HomeScreen, EpisodeBrowserScreen, JellyfinApi, and DownloadManager remain stable public classes split across concern-specific translation units.
Document the preserved UI-thread nonblocking rule and persistent-format boundaries.
Update `refactor/STATUS.md` only to mark tasks 000-036 complete if and only if the repository history shows they were completed.

## Behavior That Must Not Change

- Documentation must describe actual code, not planned code.
- No production/test changes.

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
git add docs/architecture.md refactor/STATUS.md
git commit -m "docs: document refactored architecture"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
