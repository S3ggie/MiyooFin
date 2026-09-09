# Task 17 — Activate migration on valid scope configuration

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

Make the previously validated migration path reachable through **normal application scope configuration** before any production catalog consumer switches to SQLite. A valid account/server scope automatically schedules safe worker-side database open/create/migration; the SDL/UI thread never performs or waits on migration.

## Depends On

- Task 16
- CP-B — Migration/parity approved

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- Catalog migration/schema helper files created by Tasks 14–16
- `src/app/App.hpp`
- `src/app/App.cpp`
- Focused CatalogDb/App lifecycle tests under `tests/`
- Build source lists only if required by a new catalog translation unit

## Forbidden Scope

- No SeriesScreen, EpisodeBrowser, DownloadManager, or Home hierarchy production-read switch yet.
- No LibraryCache/Home migration.
- No legacy `catalog.v1` write/delete.
- No diagnostic-only command-line activation that normal users would never execute.
- No synchronous migration/open wait from `App`, `Screen`, SDL `update`, input, or render paths.
- No production dual-write.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace every point where App gains a valid saved/login Session, logs out, or changes server/account.
- Re-read Task-04 scope epochs and Task-05 worker-only close/open behavior.
- Re-read Task-14/15 migration state machine and Task-16 parity guarantees.

## Exact implementation requirements

1. **Activation choice is fixed by this roadmap:** migration/open occurs automatically as part of the CatalogDb worker's latest valid `configureScope(serverUrl,userId)` control flow. There is no hidden diagnostic-only activation path.
2. App owns one CatalogDb service for its lifetime.
3. When App has a valid saved session, it requests CatalogDb scope configuration using that session's server URL + user ID **without waiting** before continuing the current local-first startup/Home flow.
4. When login obtains a valid session, request the new scope before/alongside normal Home transition, still nonblocking.
5. On logout, authorization rejection, account change, or server change, call `deconfigureScope()` early enough that the old epoch becomes unpublishable before old account UI/data could be reused.
6. The worker's latest valid scope activation sequence is:
   1. invalidate/cancel stale queued epochs per Task 04;
   2. finalize/close old scoped DB per Task 05;
   3. derive final/legacy paths for the new scope;
   4. if a supported valid final SQLite DB exists, open/migrate schema as needed;
   5. else if no final DB exists and legacy `catalog.v1` exists, run the validated temp import -> validation -> promotion path;
   6. else create a fresh schema DB;
   7. validate application_id/user_version/required pragmas;
   8. mark this epoch `Ready` only after the above succeeds.
7. **Before later consumer cutover tasks**, a successfully created/migrated SQLite DB is only a validated shadow/new store; existing production screens continue using the legacy paths because their call sites are unchanged.
8. Scope activation failure must leave that epoch `NotReady/Failed`; it must never fall back to the previous scope.
9. Expose a nonblocking, safe status/result sufficient for normal logs/tests/hardware observation: scope configured/opened, migration attempted, migration success/failure category. Do not log server URL, user ID, scope hash, titles, or DB path.
10. A normal-device first launch with a valid saved session and legacy catalog must therefore concretely trigger migration. A normal successful login with legacy data for that scope must also trigger it.
11. Rapid scope reconfiguration must not migrate/open superseded A/B scopes after C is already the latest requested epoch.
12. Do not automatically delete the legacy source after successful migration.

## Invariants

- Normal app lifecycle is the migration trigger.
- No UI-thread DB/migration work or waiting.
- Legacy source remains untouched.
- SQLite can be built/validated before it becomes production read authority.
- Old-scope data/results cannot publish after a new scope request.
- No production dual-write.

## Focused tests

- Valid saved session -> automatic configure -> migration/open job -> ready status.
- Valid login -> automatic scope activation.
- Logout during migration -> old epoch result suppressed and DB eventually deconfigured.
- Account A migration queued, switch to B -> A cannot become ready/publish after B request.
- Server A -> C rapid switch while import queued -> only latest scope may activate.
- Existing valid final DB -> no unnecessary legacy re-import.
- Legacy-only scope -> migration path invoked automatically.
- No legacy/no final -> fresh DB created.
- Migration failure -> scope not ready; previous scope not resurrected.
- Close/reopen same scope through logout/login.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
git status --short
```

## Commit message

```text
feat(catalog): activate scoped migration through app lifecycle
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Normal login/saved-session flow cannot trigger CatalogDb configuration without blocking the UI.
- Logout/server/account transition can expose old-scope results after the transition request.
- The only feasible migration trigger is a hidden diagnostic/tool path.
- Migration activation would make SQLite a production read authority before consumer cutover.
- Legacy catalog must be modified/deleted.

Do not broaden the task to work around a STOP condition.
