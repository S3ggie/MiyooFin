# MiyooFin Refactor Execution Plan

This directory is the execution contract for a behavior-preserving refactor of MiyooFin.

Baseline commit:

`b3b24ce8a32b48237c43a7b51e18796dd08ab20d`

Baseline validation was run before this plan was created and `make test` passed, including the
playback-runner route-aware CA check and curl/Mozilla CA bundle TLS policy check.

## Operating model

A stronger planning/review model owns architecture. The implementation model executes exactly
one numbered task at a time.

For every coding task:

1. Start from a clean worktree.
2. Read `AGENTS.md`.
3. Read `refactor/EXECUTION_RULES.md`.
4. Read only the current task file plus any plan document it explicitly references.
5. Do not start another task.
6. Do not perform unrelated cleanup.
7. Run the task's pre-change validation.
8. Make only the listed change.
9. Run all required validation.
10. Review the diff.
11. Commit exactly that task.
12. Stop and report the commit SHA.

## Reusable Codex prompt

Use this prompt and change only the task filename:

```text
Work on MiyooFin.

Read AGENTS.md and refactor/EXECUTION_RULES.md first.
Then execute ONLY:
refactor/tasks/NNN-task-name.md

Follow that task exactly.
Do not perform unrelated cleanup.
Do not start another task.
Run every required validation command.
If a required pre-change check fails, STOP and report it without changing code.
If the task cannot be completed within its Allowed Files, STOP and report why rather than widening scope.
Commit only when all acceptance criteria pass.
Then STOP and report:
- changed files
- validation commands run
- commit SHA
- any remaining concern
```

## Task order

Tasks are dependency ordered. Do not skip ahead unless a reviewer explicitly changes the plan.
`STATUS.md` is for the human/operator; implementation tasks do not edit task files.
