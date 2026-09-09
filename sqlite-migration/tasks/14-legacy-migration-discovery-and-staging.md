# Task 14 — Legacy migration discovery and staging



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

Implement safe **current-scope worker-side** detection/state handling for legacy `catalog.v1`, final SQLite DB, and disposable `.migrating` file without importing or activating it from App yet.

## Depends On

- Task 13

## Allowed Files

- CatalogDb/migration-specific new files
- Focused tests

## Forbidden Scope

- No legacy file writes.
- No production read switch.
- No actual data import yet.
- No deletion of final DB on error.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate filesystem states from `MIGRATION_SAFETY.md`.
- Inspect existing scope/path generation.

## Exact implementation requirements

1. Use only the currently ready CatalogDb scope/epoch and its existing LibraryCache-derived scope key; never accept an arbitrary caller path or cross-scope path.
2. Final path `cache/library/<scope>/catalog.sqlite3`.
3. Temporary path `catalog.sqlite3.migrating`.
4. Legacy source path remains existing OfflineCatalog path.
5. If final valid DB exists, it wins; do not import over it.
6. If only stale `.migrating` exists, classify it non-authoritative and recreate only when migration begins.
7. If both final and `.migrating` exist, final wins and temp is cleanup candidate.
8. Expose state decisions to tests without touching legacy content.

## Invariants

- Legacy immutable.
- Temp never authoritative.
- Final DB never silently replaced.

## Focused tests

- All path-state combinations.
- Permission/open errors.
- Final future-version DB preserved.
- Legacy file hash unchanged.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): stage safe legacy migration states
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- State machine could overwrite a valid final DB.
- Path resolution could cross scope boundaries.

Do not broaden the task to work around a STOP condition.
