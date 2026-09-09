# Task 26 — Remove obsolete whole-catalog RAM snapshot path



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- After the successful task commit, **STOP for mandatory human review: CP-E — Hierarchy consumer cutover**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Remove Home's reusable whole OfflineCatalogSnapshot and replace remaining runtime hierarchy projection needs with scoped CatalogDb results plus DownloadSnapshot.

## Depends On

- Task 25

## Allowed Files

- `src/ui/screens/HomeScreen.*` narrow catalog snapshot fields/helpers
- `src/cache/OfflineLibraryProjection.*` only as needed to accept scoped data or split pure helpers
- CatalogDb API
- Focused tests

## Forbidden Scope

- Do not remove OfflineCatalog files yet.
- Do not migrate LibraryCache.
- Do not change DownloadStore/ImageCache.
- No broad Home presentation redesign.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Find every `m_catalogSnapshot`, `m_catalogSnapshotReady`, `OfflineCatalog::load` runtime reference.
- Classify each as seasons/episodes/offline projection/migration-only.

## Exact implementation requirements

1. Remove Home's whole hierarchy snapshot storage/mutex if no longer needed.
2. `cachedSeasonsForSeries` becomes an already-published scoped result path or asynchronous screen query; it must not read DB on SDL thread.
3. Offline projection must operate from only needed hierarchy plus complete download metadata, or query scoped branches asynchronously.
4. No runtime path loads all hierarchy merely to filter one show/season.
5. Keep legacy OfflineCatalog reader only for migration/parity until Task 34.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Bounded memory.
- No whole hierarchy runtime snapshot.
- Local-first offline browsing preserved.
- UI thread remains DB-free.

## Focused tests

- Open show online/offline.
- Downloaded-only show list/season behavior.
- No stale snapshot race.
- Static/code-search test for forbidden runtime OfflineCatalog load call sites where appropriate.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
grep -R "m_catalogSnapshot\|OfflineCatalog::load" -n src/ui src/download || true
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
refactor(catalog): remove whole hierarchy RAM snapshot
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Offline Home semantics require whole catalog due an unmodeled behavior—stop and design a bounded query instead.
- Removing snapshot forces LibraryCache migration early.

Do not broaden the task to work around a STOP condition.
