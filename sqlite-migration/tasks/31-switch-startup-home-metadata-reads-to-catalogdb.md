# Task 31 — Switch startup/Home metadata reads to CatalogDb



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

Make CatalogDb schema v2 authoritative for cached Home/library metadata while preserving current eager presentation as an intermediate step.

## Depends On

- Task 30

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `HomeScreenSync.cpp`
- Home pure projection helpers
- CatalogDb library DAL
- App wiring
- Focused tests

## Forbidden Scope

- Do not implement final lazy paging yet.
- Do not remove LibraryCache files yet.
- No UI-thread SQLite.
- No artwork/download ownership changes.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace startup cache load, `m_cachedSnapshot`, remote snapshot save, failure/offline projection.
- Identify every LibraryCache::load/save runtime call.

## Exact implementation requirements

1. CatalogDb worker provides async cached library snapshot/projection for intermediate parity.
2. Home loading screen/presentation must not block waiting for DB.
3. Network library refresh persists authoritative view/Home metadata in one bounded catalog job/transaction before publishing success state.
4. Stop writing the LibraryCache snapshot after successful cutover; retain it read-only only for supported rollback/compatibility handling until Task 34.
5. Network failure preserves last valid SQL cached presentation.
6. Stale-generation snapshot semantics still force refresh as current behavior requires.
7. Checkpoint/hierarchy behavior remains unchanged.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Local-first.
- No UI DB.
- No production dual-write.
- Current Home appearance/order unchanged at this step.

## Focused tests

- Cached startup.
- No cache.
- Network failure with cache.
- Fresh remote refresh.
- Offline/manual-offline mode.
- Fresh DB with no snapshot.
- Network failure with DownloadStore-backed offline fallback.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
grep -R "LibraryCache::save" -n src || true
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(home): make CatalogDb authoritative for library metadata
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Startup must synchronously wait on SQLite.
- Home loses valid cached content on network failure.
- Legacy and SQLite caches would both be production-written.

Do not broaden the task to work around a STOP condition.
