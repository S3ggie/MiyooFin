# Task 05 — SQLite connection ownership and prepared-statement registry



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

Open exactly one SQLite connection on the CatalogDb worker **only for the currently configured server/user scope**, and establish worker-only statement ownership/reuse infrastructure.

## Depends On

- Task 04

## Allowed Files

- `src/catalog/CatalogDb.*`
- New `src/catalog/CatalogDbSql.*` or internal statement helper
- Focused tests
- Build files only if new source units are added

## Forbidden Scope

- No media schema beyond a minimal test DB/open if needed.
- No screen integrations.
- No second connection.
- No busy-wait loop.
- No page/cache/mmap tuning.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Prove worker thread identity can be asserted in debug/tests.
- Review approved runtime baseline in `SQLITE_CONFIGURATION.md`.

## Exact implementation requirements

1. Open/close the SQLite connection only from the CatalogDb worker, and only while processing the current non-stale configured scope epoch.
2. Use `sqlite3_open_v2` with flags appropriate for read/write/create and no shared-cache mode.
3. Enable extended result codes.
4. Set/verify `foreign_keys=ON` and `trusted_schema=OFF`.
5. Set initial `journal_mode=DELETE`, `synchronous=FULL`, `locking_mode=NORMAL`.
6. Do not set page_size/cache_size/temp_store/mmap/wal_autocheckpoint.
7. Create a statement registry owned by the connection; statements prepare once and reset/clear-bindings for reuse.
8. On scope switch/deconfigure: finalize all old prepared statements, close the old scoped connection, clear path/schema-ready state, then (only for the latest valid epoch) open the new scoped database path. Never have two scoped connections open simultaneously.
Derive the DB path from the configured LibraryCache scope key; no DB open is allowed while unconfigured.
A scope-open failure leaves that epoch not-ready and must not fall back to the previous scope.
Finalize all prepared statements before connection close.
9. Expose safe error categories/result codes to CatalogDb, not raw sqlite handles.
10. Add worker-thread assertions/tests that reject accidental non-worker SQLite use.

## Invariants

- One connection total across all scopes; scope switch closes old before new opens.
- Worker-only sqlite3/sqlite3_stmt ownership.
- Prepared statements reusable.
- Baseline journal profile only.

## Focused tests

- Open/close/reopen temp DB.
- Required pragmas verified.
- Statement prepare/reuse/finalize lifecycle.
- Worker-thread ownership guard.
- SQLite errors propagated.
- Scope A close -> scope B open; A statements finalized.
- Logout/deconfigure closes connection and ordinary jobs cannot access A.
- Close/reopen same scope.
- Rapid A -> B -> C only opens latest non-stale target or closes intermediate targets before proceeding.
- A/B databases seeded with different sentinel rows never cross-publish.

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
feat(catalog): own SQLite connection on CatalogDb worker
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Any SQLite call is required from UI thread.
- A second connection appears necessary.
- Target VFS rejects baseline DELETE/FULL during ARM/runtime smoke tests—stop for review, do not switch journal mode ad hoc.

Do not broaden the task to work around a STOP condition.
