# Task 000 — Add refactor runner rules to root AGENTS.md

## Status

NOT STARTED

## Depends On

None

## Goal

Add refactor runner rules to root AGENTS.md. This task must be behavior-preserving.

## Allowed Files

- `AGENTS.md`

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

Append a short `## Refactor task execution` section to the existing root `AGENTS.md`.
Do not rewrite or delete existing project instructions.

The new section must say:
- numbered tasks live under `refactor/tasks/`;
- execute exactly one task at a time;
- `refactor/EXECUTION_RULES.md` is mandatory;
- Allowed Files are a hard boundary;
- run every validation command;
- one task equals one commit;
- stop after the commit and never auto-start the next task.

## Behavior That Must Not Change

- Existing AGENTS.md rules remain intact.
- No production or test source changes.

## Focused Validation

Run:

```sh
`grep -n "Refactor task execution" AGENTS.md`
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
git add AGENTS.md
git commit -m "docs: add refactor task execution rules"
git status --short
```

If an Allowed File did not change, do not force-add or manufacture a change.

## STOP

After the commit succeeds, STOP.
Do not read or execute the next task.
Report the changed files, validation commands, commit SHA, and any concern.
