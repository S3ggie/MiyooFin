# Task 23 — Switch DownloadManager hierarchy planning to CatalogDb



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

Use CatalogDb for cached series/season expansion while keeping DownloadManager/DownloadStore fully authoritative for download state and bytes.

## Depends On

- Task 22

## Allowed Files

- `src/download/DownloadManagerPlanning.cpp`
- `src/download/DownloadManager.hpp` only for narrow async integration
- CatalogDb public API
- App wiring if manager needs CatalogDb reference
- Focused tests

## Forbidden Scope

- No DownloadStore format changes.
- No transfer/reconcile/HLS changes.
- No playback resolver change.
- No FK from downloads to catalog.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace `requestSeriesPlan`, `requestSeasonPlan`, planner worker, and cached estimate behavior.
- Define safe service lifetime/reference ownership from App.

## Exact implementation requirements

1. Cached hierarchy estimate comes from CatalogDb query jobs, never OfflineCatalog full load.
2. Planner's network hierarchy discovery persists through CatalogDb.
3. Download plan generation continues on DownloadManager's owned planner worker.
4. Do not hold DownloadManager mutex while waiting/blocking on CatalogDb.
5. If async coordination is needed, use job continuation/result publication without SDL blocking.
6. DownloadStore remains authoritative for local bytes/manifests.
7. Catalog deletion/rebuild cannot delete download records.
8. Local playback resolution remains `hasComplete()` based.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Download ownership unchanged.
- No lock inversion CatalogDb <-> DownloadManager.
- No UI DB.
- No legacy catalog write.

## Focused tests

- Cached series estimate.
- Cached season estimate.
- Network fallback planning.
- Catalog missing but download metadata survives.
- Concurrent manager/catalog operations no deadlock.

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
feat(downloads): plan hierarchy through CatalogDb
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Design requires DownloadManager mutex held across CatalogDb wait.
- Catalog becomes authoritative for downloaded bytes.
- Transfer/reconcile code must be refactored.

Do not broaden the task to work around a STOP condition.
