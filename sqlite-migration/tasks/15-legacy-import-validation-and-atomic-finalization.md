# Task 15 — Legacy import validation and atomic finalization



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

Import OfflineCatalog v1 into a temporary SQLite DB, validate it, and atomically promote only a clean closed database.

## Depends On

- Task 14

## Allowed Files

- Catalog migration files
- Existing OfflineCatalog read APIs only as source
- CatalogDb/schema/codec helpers
- Focused tests

## Forbidden Scope

- Do not change legacy serializer format.
- Do not write/delete `catalog.v1`.
- No production dual-write.
- No screen integration.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Create byte/hash checks proving source immutability.
- Review migration state machine and SQLite sidecar behavior.

## Exact implementation requirements

1. Read the legacy snapshot for the worker's current non-stale scope epoch as source; re-check epoch before promotion so a superseded scope cannot publish/promote as current.
2. Create fresh `.migrating` schema v1.
3. Import series/seasons/episodes using transactionally safe routines.
4. Set hierarchy completeness consistent with legacy snapshot semantics.
5. Validate row counts and canonical MediaItem parity.
6. Run `PRAGMA quick_check` (or stronger host test where practical) and `PRAGMA foreign_key_check`.
7. Close/finalize the temporary connection cleanly before promotion.
8. Ensure no live journal/WAL family is left for the temp DB.
9. `fsync` completed temp DB and rename to final path; perform best-effort directory durability compatible with target VFS/filesystem.
10. Reopen final DB for the same current epoch and verify application_id/user_version and representative counts. If the epoch was superseded, close it and suppress readiness/result publication.
11. On any failure, leave legacy untouched and final absent/unchanged.

## Invariants

- Source immutable.
- Promotion only after semantic + structural validation.
- No live DB rename.
- Failure is retryable.

## Focused tests

- Successful import.
- Corrupt/truncated legacy source.
- Injected SQL failure.
- Injected validation failure.
- Interrupted temp file simulated across restart.
- Legacy hash unchanged in every failure case.

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
feat(catalog): import legacy catalog atomically
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Promotion needs renaming an open/live SQLite DB.
- Validation cannot prove source parity.
- Legacy file must be modified to proceed.

Do not broaden the task to work around a STOP condition.
