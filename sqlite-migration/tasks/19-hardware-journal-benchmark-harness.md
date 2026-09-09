# Task 19 — Hardware journal benchmark harness



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- In autonomous roadmap mode, after successful validation and the task commit, immediately open the next numbered task and continue without asking for routine confirmation.

## Goal

Create a benchmark-only harness that exercises the same schema/SQLite build and controlled hierarchy workloads under each candidate journal profile without changing the production baseline.

## Depends On

- Task 18
- CP-C approval

## Allowed Files

- `tools/**` benchmark utility
- Catalog internal test/benchmark seams
- `Makefile`/`Makefile.cross` benchmark targets
- Telemetry analyzer support if needed
- Tests

## Forbidden Scope

- Production CatalogDb default remains DELETE+FULL.
- No permanent WAL switch.
- No UI changes.
- No synthetic benchmark that bypasses the real vendored SQLite build/schema.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Review `BENCHMARK_PROTOCOL.md`.
- Ensure harness can target `/mnt/SDCARD` path safely without real user catalog.

## Exact implementation requirements

1. Support exactly the six candidate profiles A–F.
2. Create fresh benchmark DB per profile/run under an isolated path.
3. Replay deterministic typical/large/generation/delete workloads using same schema and write routines where practical.
4. Report durations, row counts, result codes, DB/journal/WAL/shm sizes.
5. Support process interruption/recovery fixtures.
6. Never point destructive benchmark operations at production catalog/download paths.
7. Build for host and ARM.

## Invariants

- Benchmark isolation.
- Production profile unchanged.
- Same sqlite3.c/schema.

## Focused tests

- All profiles accepted on host where supported.
- Unknown profile rejected.
- Isolation path guard.
- Results deterministic by fixture.

## Complete validation commands

```sh
make test -j2
make -j2
# run host sqlite catalog benchmark target for A-F
make onionos
make verify-arm
# build ARM benchmark target
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
tools(sqlite): add catalog journal benchmark harness
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Harness requires changing normal runtime profile.
- Benchmark path can collide with production cache.
- Workload does not exercise same schema/write code.

Do not broaden the task to work around a STOP condition.
