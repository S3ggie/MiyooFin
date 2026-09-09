# Task 28 — Schema v2 for LibraryCache/Home metadata



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

After hierarchy-only hardware approval, add schema v2 tables for library views/membership/Home rows without switching consumers yet.

## Depends On

- Task 27
- CP-F approval

## Allowed Files

- Catalog schema/version files
- CatalogDb statement registry
- Focused tests
- `SCHEMA_V1.md` should not be rewritten as v2; add production schema-v2 docs in repo if implementation policy requires

## Forbidden Scope

- No startup/Home consumer switch.
- No lazy paging yet.
- No LibraryCache deletion.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Re-read current LibrarySnapshot/CachedLibraryView semantics.
- Re-read Movie/Shows organizational ordering helpers.

## Exact implementation requirements

1. Transactional v1->v2 migration.
2. Add library_views(id PK,name,collection_type,ordinal).
3. Add library_membership(view_id,item_id,ordinal) with FK cascades and view-order index plus reverse item index.
4. Add home_items(row_kind,item_id,ordinal) with row-order index.
5. Reuse media_items for movies/shows/Home-row MediaItems; no duplicate metadata table.
6. Do not add organizational sort keys yet unless Task 30 parity proves needed.
7. Set user_version=2 only on successful migration.
8. Rebuild prepared statement registry after migration.

## Invariants

- Hierarchy v1 data preserved.
- No Home behavior change.
- One media identity table.

## Focused tests

- v1->v2 migration preserves hierarchy hash/parity.
- Fresh v2 creation.
- Rollback failure leaves v1.
- FK/index definitions.

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
feat(catalog): add library metadata schema v2
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Migration damages v1 hierarchy.
- LibrarySnapshot semantics require duplicated item authority.
- Organizational ordering cannot be represented later without destructive schema change—review before proceeding.

Do not broaden the task to work around a STOP condition.
