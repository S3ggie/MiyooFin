# Task 08 — Enforce one CatalogDb scope owner

**Phase:** A — correctness

## Objective

Remove HomeScreen's fallback `configureScope()` so App/session lifecycle is the only current production scope owner.

## Why

Home can currently advance the CatalogDb epoch itself even though App already owns scope lifecycle.

## Preconditions

- Task 07 works with the App-provided scope epoch.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `src/app/App.cpp` and `.hpp` only for a narrow epoch guard/handoff
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_ui_foundation.inc`

## Forbidden Scope

- Do not move scope ownership into a screen.
- Do not make configure synchronous.
- Do not change one-connection lifecycle.
- Do not extract LibrarySync yet.

## Architecture invariants

- Schema v3 remains the baseline. Do not introduce schema v4 or redesign the schema unless this task explicitly requires it; no task in this roadmap currently does.
- Exactly one `CatalogDb` SQLite worker owns exactly one SQLite connection for the active scope.
- No SQLite operation, HTTP request, long filesystem operation, retry sleep, or blocking worker join may run on the SDL/UI thread.
- Keep indexed/keyset bounded reads and bounded result windows; do not reintroduce whole-library RAM materialization.
- Keep the atomic fresh-database temporary-file bootstrap/promotion path unchanged unless a task explicitly targets it.
- `DownloadStore` remains authoritative for physical offline availability. Catalog metadata may enrich downloads but must not decide whether bytes exist.
- `ImageCache` remains the artwork-byte cache and is not replaced by SQLite.
- Do not add production dual-write to legacy whole-file catalog/cache persistence.
- The intermittent audio-only FFplay/mmiyoo display bug is out of scope. Do not modify playback repair code in this roadmap.

## Implementation requirements

1. Delete any Home fallback call to `CatalogDb::configureScope()`.
2. A zero/mismatched epoch is treated as not-ready/superseded; Home never repairs it by reconfiguring the DB.
3. App saved-session/login/logout/server/account switch remain the only production configure/deconfigure paths for now.
4. Repeated Home paging must not advance requested scope epoch.
5. Logout/scope switch continues to invalidate old results.

## Tests

- Valid Home paging leaves requested epoch unchanged.
- Zero epoch causes no configure request from Home.
- Existing scope-switch/supersession tests still pass.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
```

## Hardware gate

None. This task is host/ARM-build validation only; do not deploy to a Miyoo Mini Plus.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
refactor(catalog): keep scope lifecycle out of Home
```

Do not include any part of Task 09 in this commit.
