# Task 24 — Switch Home hierarchy persistence to incremental SQLite



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

Replace Home hierarchy worker's full OfflineCatalog reconcile/load/save/reload cycle with CatalogDb incremental transactions.

## Depends On

- Task 23

## Allowed Files

- `src/ui/screens/HomeScreenHierarchy.cpp`
- `HomeScreen.hpp` narrow fields
- `HomeScreenSync.cpp` if needed for job handoff
- CatalogDb API
- Focused tests

## Forbidden Scope

- Do not move checkpoint yet—that is Task 25.
- Do not remove m_catalogSnapshot yet—that is Task 26.
- Do not migrate LibraryCache.
- No Home rendering/navigation refactor.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace generation supersession, completed/total counters, offline flag, and current checkpoint conditions.
- Map authoritative top-level series reconciliation and per-series writes to CatalogDb jobs.

## Exact implementation requirements

1. Top-level authoritative series list triggers CatalogDb reconciliation instead of OfflineCatalog::reconcileSeries.
2. Per complete fetched series triggers atomic UpsertSeriesHierarchy.
3. Remove full-file save/reload from hierarchy worker.
4. Maintain one series as natural write transaction.
5. Generation supersession/cancellation semantics remain.
6. A series counts completed only after its SQL commit succeeds.
7. Failure keeps generation incomplete.
8. Keep existing checkpoint file behavior temporarily in Task 24 so cutover is separable from Task 25.
9. Do not build a fresh whole DB snapshot after each series.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Completed counter means committed.
- Incomplete network fetch never persisted authoritative.
- Checkpoint behavior unchanged until next task.
- No UI DB.

## Focused tests

- Generation all success.
- One series SQL failure.
- Network failure.
- Superseded generation.
- Authoritative deleted series.
- No full catalog file write.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(home): persist hierarchy incrementally in SQLite
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Current generation counters can advance before commit.
- Checkpoint file must be redesigned simultaneously to work—stop for review.
- Home requires full DB snapshot to continue.

Do not broaden the task to work around a STOP condition.
