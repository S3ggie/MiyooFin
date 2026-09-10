# Task 29 — LibraryCache snapshot seed



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

Seed schema v2 from the separate `LibraryCache` `snapshot.v1` metadata snapshot
while keeping that snapshot untouched. This is independent of the retired
`catalog.v1` hierarchy-import strategy.

## Depends On

- Task 28

## Allowed Files

- New/updated catalog schema/seed files
- Existing LibraryCache read API as source
- Focused tests

## Forbidden Scope

- No production Home read switch.
- No writes to snapshot.v1.
- No permanent dual-write.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate LibraryCache versions 1..current and `needsRefresh` semantics.
- Define how older cache generations are imported/marked for network refresh.

## Exact implementation requirements

1. Read legacy LibraryCache snapshot read-only.
2. UPSERT movie/show/Home-row MediaItems into media_items.
3. Replace library_views/membership/home_items atomically from the authoritative snapshot.
4. Preserve view order and item order.
5. Preserve Continue Watching and Recently Added order.
6. Seed stale-generation snapshot only as local data while retaining/setting refresh-needed state outside destructive schema semantics.
7. Do not delete hierarchy rows not present in LibrarySnapshot.
8. Validate semantic counts and canonical item parity.

## Invariants

- LibraryCache snapshot immutable.
- Hierarchy survives.
- No duplicate item authority.
- No co-write.

## Focused tests

- Current snapshot.
- Older supported snapshot needing refresh.
- Multiple views.
- Items shared with hierarchy/Home rows.
- Empty rows/views.
- Failure rollback.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): seed library snapshot into SQLite
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Import would delete hierarchy-only metadata.
- Old snapshot version cannot be interpreted safely.
- Need to mutate legacy snapshot.

Do not broaden the task to work around a STOP condition.
