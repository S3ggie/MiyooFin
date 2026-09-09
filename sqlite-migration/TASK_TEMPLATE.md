# Task NN — Title

## Execution mode — roadmap override

- Main/current Codex performs implementation directly with GPT-5.6 Luna High.
- No implementation subagents.
- No reviewer subagents by default.
- No SDD workspaces, generated briefs, ledgers, or orchestration artifacts.
- One task = one commit.
- This task file's Commit message authorizes exactly that task commit after validation.
- In autonomous roadmap mode continue to the next ordinary task automatically.
- STOP at a mandatory human checkpoint or genuine STOP condition.

## Goal

One narrow outcome.

## Depends On

- Task XX
- Checkpoint YY if applicable

## Allowed Files

- Exact edit boundaries.

## Forbidden Scope

- Adjacent systems that must not change.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

## Exact implementation requirements

1. Requirement.

## Invariants

- Invariant.

## Focused tests

- Test.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

For ARM/runtime-affecting changes:

```sh
make onionos
make verify-arm
```

## Commit message

```text
type(scope): narrow task description
```

The task file is explicit authorization to make exactly one commit after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- condition.

Do not broaden scope to work around a STOP condition.
