# Task 25 — Move hierarchy completion checkpoint to SQLite



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

Make SQLite `sync_state` authoritative for hierarchy completion watermark while preserving complete-generation-only advancement.

## Depends On

- Task 24

## Allowed Files

- `src/ui/screens/HomeScreenHierarchy.cpp`
- `HomeScreen.cpp`/`HomeScreenSync.cpp` narrow sync-state load/use
- CatalogDb sync-state API
- `src/cache/SyncState.*` only for temporary migration compatibility
- Focused tests

## Forbidden Scope

- No LibraryCache migration.
- Do not delete SyncStateStore yet.
- No per-series checkpoint writes.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate every SyncStateStore read/write call.
- Model failures between final series commit and checkpoint commit.

## Exact implementation requirements

1. Import existing sync-state timestamps into SQLite once when DB sync row has no migrated value; source remains untouched.
2. Read hierarchy checkpoint asynchronously/off-thread before ChangedHierarchy requests need it; integrate without blocking SDL.
3. After generation is fully committed/current/online, enqueue one final checkpoint transaction.
4. Checkpoint update contains lastSuccessfulMs, conditional lastReconcileMs, committedGeneration.
5. No series transaction advances checkpoint.
6. If final checkpoint transaction fails, generation data may remain newer but old checkpoint remains, causing conservative reprocessing.
7. Retain legacy SyncStateStore only as compatibility source until final retirement, not a production co-writer.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- All-or-old watermark.
- No dual-write.
- Checkpoint failure never claims success.
- Reprocessing is safe/idempotent.

## Focused tests

- All success advances once.
- Series failure no advance.
- Generation superseded no advance.
- Checkpoint SQL failure leaves old value.
- Crash after data commit before checkpoint -> old checkpoint.
- Existing sync-state compatibility seed one-time semantics.

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
feat(catalog): store hierarchy completion checkpoint in SQLite
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Any path advances checkpoint per series.
- UI must synchronously query checkpoint.
- Legacy and SQLite checkpoints would both be written indefinitely.

Do not broaden the task to work around a STOP condition.
