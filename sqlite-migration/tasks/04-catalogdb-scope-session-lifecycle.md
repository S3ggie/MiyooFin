# Task 04 — CatalogDb scope/session lifecycle

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

Define the app-scoped CatalogDb service's server/user scope lifecycle before any database-opening or DAL work depends on it. A scope change must invalidate old work/results immediately, while all close/open operations remain worker-owned.

## Depends On

- Task 03

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- New catalog-internal scope/job types under `src/catalog/`
- Focused CatalogDb tests under `tests/`
- Build source lists only if a new catalog translation unit is added

## Forbidden Scope

- Do not wire CatalogDb into `App`/login/logout yet; production activation is Task 17.
- No SQLite connection open/close yet; that ownership is Task 05.
- No schema, migration, MediaItem DAL, screens, DownloadManager, LibraryCache persistence, or telemetry changes.
- Do not move or change the existing `LibraryCache::scopeKey` algorithm.
- Do not store access tokens, usernames, or other credentials in CatalogDb scope state.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Re-read the existing `LibraryCache::scopeKey(serverUrl,userId)` behavior.
- Re-read CatalogDb queue scheduling/cancellation from Tasks 02–03.
- Confirm all queued jobs can carry an immutable scope epoch without capturing screen-owned references.

## Exact implementation requirements

1. Add a nonblocking control API shaped like `configureScope(serverUrl, userId)` and `deconfigureScope()`; exact names may vary only if semantics remain identical.
2. A valid requested scope derives the **existing LibraryCache server/user scope key** from `serverUrl` + `userId`. Empty/invalid identity must not create a configured scope.
3. CatalogDb begins **unconfigured**. No normal scoped job may be accepted/executed as though a DB exists before a valid scope is requested.
4. Maintain an atomic/request-visible **scope epoch**. Every configure/deconfigure request increments it immediately before the worker performs slow close/open work.
5. Every scope-bound queued job is tagged with the epoch captured at enqueue.
6. As soon as a newer configure/deconfigure request advances the requested epoch:
   - late results from older epochs must be suppressed;
   - queued old-epoch work becomes cancellable/rejected;
   - newly submitted work belongs only to the new requested epoch.
7. Scope-control work has precedence over ordinary queued DB jobs. It may not interrupt an already-running atomic job, but no old-scope queued job may start after the worker begins processing the newer scope control.
8. On the worker, skip superseded configure requests. For rapid `A -> B -> C`, do not unnecessarily activate A or B if their epochs are already stale when dequeued.
9. Deconfiguration represents logout/no valid account. It invalidates old results immediately and leaves CatalogDb with no configured scope.
10. Add safe observable state for tests/callers: requested epoch, ready/not-ready state, and success/failure category. Do not expose the raw scope key as UI content.
11. Result publication APIs must require both job epoch == current requested epoch and scope still ready for that epoch.
12. No database row/data from scope A may be publishable after scope B/C has been requested, even if A work completes successfully.

## Invariants

- App-scoped service; server/user-scoped logical catalog.
- Scope identity uses the existing LibraryCache scope derivation.
- Scope epoch changes immediately on reconfiguration request.
- Old-scope result publication is impossible after a new scope request.
- No SQLite work yet.
- No UI blocking and no detached threads.

## Focused tests

- Initial unconfigured service rejects ordinary scoped jobs.
- Configure valid scope A produces epoch A.
- `deconfigureScope()` invalidates A and suppresses a late A result.
- Account switch A -> B changes epoch and rejects/cancels queued A work.
- Server switch A -> C changes epoch even when user ID is unchanged.
- Jobs queued during a switch are either bound to the new epoch or explicitly rejected; none run under the old scope accidentally.
- Rapid A -> B -> C reconfiguration leaves only C publishable and skips superseded scope controls.
- Stale callback/result from A is suppressed after B request.
- Reconfigure to the same exact scope still has defined/idempotent behavior with tests (either no-op without epoch change or deliberate epoch refresh; choose one and document it).

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
git status --short
```

## Commit message

```text
feat(catalog): add scoped CatalogDb lifecycle epochs
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Scope switching cannot suppress old results without blocking the caller.
- Queue/job objects cannot be safely epoch-tagged without broad screen refactors.
- Correct scoping would require changing the LibraryCache scope-key algorithm.
- A design would permit rows/results from one server/user scope to be observed after another scope is requested.

Do not broaden the task to work around a STOP condition.
