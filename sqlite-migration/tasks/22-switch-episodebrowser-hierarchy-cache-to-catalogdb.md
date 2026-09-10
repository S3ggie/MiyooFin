# Task 22 — Switch EpisodeBrowser hierarchy cache to CatalogDb



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

Replace EpisodeBrowser's whole-catalog episode load/store with async indexed season queries and CatalogDb persistence.

## Depends On

- Task 21

## Allowed Files

- `src/ui/screens/EpisodeBrowserScreen.*` and narrow related EpisodeBrowser units
- `src/ui/screens/SeriesScreen.cpp` — only the existing
  `EpisodeBrowserScreen` construction callsite needed to propagate the
  app-scoped `CatalogDb`.
- `src/ui/screens/HomeScreenNavigation.cpp` — only the existing direct
  `EpisodeBrowserScreen` construction callsite, which must receive the same
  service and scope metadata.
- CatalogDb public API
- `tests/cases/test_cache_offline.inc` — focused EpisodeBrowser/Series wiring
  coverage.

The wiring additions must not create another `CatalogDb` instance or move
ownership. They are limited to constructor propagation and must not perform
SQLite work on the SDL/UI thread.

## Forbidden Scope

- No DownloadManager planning change.
- No Home hierarchy change.
- No artwork/playback behavior redesign.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace current cached-first publish and initialEpisode selection.
- Preserve artwork worker pause/cancellation.

## Exact implementation requirements

1. InteractiveRead GetEpisodes(seasonId) replaces OfflineCatalog load.
2. Cached results publish through existing async state, not SDL blocking.
3. Successful network episode fetch persists one season/series hierarchy update safely; do not claim entire series completeness if only one season is refreshed—use a DAL operation whose authoritative scope is that season.
4. Do not write legacy catalog.
5. Downloaded-only/offline filtering remains based on complete DownloadSnapshot/metadata fallback.
6. Preserve initial episode selection and playback fields.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- No UI DB.
- Season-level authoritative scope only.
- Playback/download actions unchanged.

## Focused tests

- Cached offline episodes.
- Initial episode selection.
- Network refresh.
- Cancellation.
- Downloaded-only parity.
- Season update removes stale episode only within that season.

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
feat(episodes): read cached hierarchy from CatalogDb
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Existing DAL cannot safely express season-scoped authoritative update—add a narrowly reviewed DAL extension, do not abuse full-series complete flag.
- Playback/download state changes unexpectedly.

Do not broaden the task to work around a STOP condition.
