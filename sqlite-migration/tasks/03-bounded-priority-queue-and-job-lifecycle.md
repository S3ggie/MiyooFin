# Task 03 — Bounded priority queue and job lifecycle



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

Add the bounded priority CatalogDb job queue, cancellation/generation metadata, and explicit enqueue failure semantics without adding database work.

## Depends On

- Task 02

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- New catalog-internal job/queue headers if needed
- Focused tests

## Forbidden Scope

- No SQLite API calls.
- No screen/DownloadManager integration.
- No schema.
- No generic repository-wide threading abstraction.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Inspect existing queue-depth telemetry patterns but do not instrument SQLite yet.
- Choose and document a small fixed queue capacity based on expected producers; make it a named constant, not an unbounded container.

## Exact implementation requirements

1. Define priorities: InteractiveRead, ForegroundMetadataWrite, BackgroundSync, Maintenance.
2. FIFO ordering within each priority.
3. Bound total pending jobs.
4. Enqueue must be nonblocking and return explicit accepted/rejected result.
5. Represent cancellation/generation in jobs without capturing unsafe screen references.
6. Worker always finishes the current atomic job before moving to another.
7. Add fairness protection so sustained BackgroundSync cannot prevent InteractiveRead from being selected next; avoid complex scheduling beyond this requirement.
8. On shutdown, define which queued jobs are cancelled and prove callbacks/results are not published into destroyed owners.

## Invariants

- Bounded queue.
- No blocking producer wait.
- Priority cannot split a running transaction/job.
- No unsafe pointer/reference capture.

## Focused tests

- Capacity rejection.
- FIFO per class.
- InteractiveRead preferred over queued background jobs.
- Shutdown with full queue.
- Cancellation before execution.
- Generation mismatch suppression.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): add bounded priority job queue
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Queue needs unbounded growth to preserve behavior.
- A required caller would need to block until queue space exists.
- Result publication cannot be lifetime-safe.

Do not broaden the task to work around a STOP condition.
