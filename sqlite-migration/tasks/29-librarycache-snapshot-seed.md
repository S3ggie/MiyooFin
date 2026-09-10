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

- `src/catalog/CatalogDb.hpp` — the smallest public asynchronous snapshot-seed
  request/result API, including scope-epoch metadata and bounded result
  reporting.
- `src/catalog/CatalogDb.cpp` — the matching worker-owned job dispatch and one
  bounded transactional seed implementation, using the existing CatalogDb
  worker and connection.
- `src/cache/LibraryCache.hpp` and `src/cache/LibraryCache.cpp` — only the
  read-only snapshot parsing/adapter surface needed to pass validated
  `LibrarySnapshot` contents to the seed job; snapshot writes remain forbidden.
- `tests/test_main.cpp` and the focused catalog/cache test case include(s) —
  successful seed, idempotency, rollback, representative parity, empty
  snapshot, and stale-scope rejection tests.
- `sqlite-migration/tasks/29-librarycache-snapshot-seed.md` — this task
  specification only, if implementation details need clarification.

No other production or consumer files are in scope. In particular, the
existing schema-v2 files may be read but must not be expanded for unrelated
tables or consumers.

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
9. Expose this only as one asynchronous CatalogDb seed job. It must use the
   existing single worker/connection, carry the requested scope epoch, reject
   stale jobs before publication, and never execute SQLite on the SDL/UI
   thread.
10. Make the seed idempotent: repeated seeds replace the scoped view,
    membership, and Home-row projections without duplicate rows while leaving
    hierarchy-only rows intact.
11. Commit the bounded seed transaction only after all snapshot validation and
    writes succeed; a failure must roll back the complete seed.
12. Do not add permanent dual-write behavior, switch Home reads, alter
    DownloadStore, alter hierarchy persistence, or modify unrelated consumers.

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
- Repeated seed produces identical row counts, order, and canonical metadata.
- A stale scope epoch is rejected without SQLite writes or publication.

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
