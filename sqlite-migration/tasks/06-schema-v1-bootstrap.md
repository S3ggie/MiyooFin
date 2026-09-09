# Task 06 — Schema v1 bootstrap



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

Implement creation of the hierarchy-first schema exactly as defined in `SCHEMA_V1.md`.

## Depends On

- Task 05

## Allowed Files

- `src/catalog/CatalogDb.*`
- New `src/catalog/CatalogSchema.*`
- Focused schema tests
- Build files for new source

## Forbidden Scope

- No LibraryCache/Home tables.
- No consumer switch.
- No legacy import.
- No schema-v2 work.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Re-read `SCHEMA_V1.md`.
- Check SQLite application-ID registry before choosing the numeric application ID; document the checked source/date.

## Exact implementation requirements

1. Create v1 tables: media_items, item_genres, item_image_tags, hierarchy_state, sync_state.
2. Create only the two initial hierarchy indexes from `SCHEMA_V1.md`.
3. Seed sync_state singleton row 1.
4. Set noncolliding application_id and `user_version=1` only after successful schema creation.
5. Create schema in one transaction on a new DB.
6. Verify FK enforcement.
7. Do not populate library/Home data.

## Invariants

- Jellyfin IDs are media PKs.
- Genres/image tags normalized.
- No download/artwork bytes.
- Schema v1 hierarchy-only.

## Focused tests

- Fresh schema exact objects/indexes.
- FK constraints/cascades.
- CHECK constraints.
- Application ID/user_version.
- Repeated open does not recreate destructively.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): add hierarchy catalog schema v1
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Application ID collision is found.
- Legacy data shape obviously cannot satisfy relationships.
- Schema needs LibraryCache data to function.

Do not broaden the task to work around a STOP condition.
