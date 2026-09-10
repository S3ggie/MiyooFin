# Task 21 — Switch SeriesScreen hierarchy cache to CatalogDb



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

Replace SeriesScreen's whole OfflineCatalog load/store with asynchronous CatalogDb season reads and atomic hierarchy persistence.

## Depends On

- Task 20
- CP-D approval

## Allowed Files

- `src/ui/screens/SeriesScreen.*`
- `src/ui/screens/HomeScreenNavigation.cpp` — only the existing
  `SeriesScreen` construction callsites needed to pass the app-scoped
  `CatalogDb` service already owned by `HomeScreen`.
- CatalogDb public job API
- `tests/cases/test_cache_offline.inc` — focused SeriesScreen/navigation
  coverage for the propagated service and asynchronous behavior.

The wiring change must not create another `CatalogDb` instance or move service
ownership. `HomeScreenNavigation.cpp` may only propagate the existing service;
it must not perform SQLite work or broaden the consumer cutover.

## Forbidden Scope

- No EpisodeBrowser changes.
- No DownloadManager changes.
- No Home hierarchy changes.
- No UI-thread SQLite.
- No rendering/layout refactor.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace current cached-first/network-refresh SeriesScreen lifecycle and cancellation.
- Preserve selection/artwork behavior.

## Exact implementation requirements

1. `enter()` remains immediately usable/loading without DB blocking.
2. Submit InteractiveRead GetSeasons when cached seasons were not supplied.
3. Publish completed cached seasons on the screen's existing async result path.
4. Network refresh remains on existing worker.
5. On successful complete network seasons response, persist via CatalogDb job; never write legacy catalog.
6. Do not wait synchronously on DB from SDL thread.
7. Downloaded-only/offline mode must continue to filter playability through DownloadManager snapshot/fallback semantics.
8. Preserve cancellation/lifetime rules.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Local-first.
- No UI DB.
- No legacy write.
- Selection behavior preserved.

## Focused tests

- Cached seasons appear before/without network.
- Offline series browsing.
- Network refresh update.
- Cancellation/back navigation.
- Downloaded-only projection parity.

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
feat(series): read cached hierarchy from CatalogDb
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- SeriesScreen must block waiting for DB.
- Downloaded-only behavior regresses.
- Network incomplete response would be persisted as authoritative.

Do not broaden the task to work around a STOP condition.
