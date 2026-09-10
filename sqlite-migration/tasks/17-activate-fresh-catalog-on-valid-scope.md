# Task 17 — Activate fresh CatalogDb on valid scope configuration

## Execution mode — roadmap override

Implement directly with the selected GPT-5.6 Luna High. Do not spawn subagents
or orchestration artifacts. One task equals one narrow commit; preserve all other
repository safety, threading, hardware-evidence, and validation rules.

## Goal

Make fresh CatalogDb bootstrap reachable through normal application scope
configuration before SQLite becomes a production read authority. A valid saved
session or successful login opens an existing scoped DB or creates a fresh one;
network reconciliation and offline DownloadStore reconstruction then provide the
appropriate initial contents without consulting `catalog.v1`.

## Depends On

- Task 16
- CP-B — Fresh bootstrap/reconciliation/offline-download behavior approved

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- Catalog bootstrap/reconciliation/offline helper files from Tasks 14–16
- `src/app/App.hpp`
- `src/app/App.cpp`
- Focused CatalogDb/App lifecycle tests under `tests/`
- Build source lists only if required by a new catalog translation unit

## Forbidden Scope

- No SeriesScreen, EpisodeBrowser, DownloadManager, or Home production read switch.
- No `catalog.v1` write, delete, parse, or import.
- No synchronous bootstrap/reconciliation wait from App, Screen, input, update,
  or render paths.
- No production dual-write.

## Exact implementation requirements

1. App owns one CatalogDb service for its lifetime and calls `configureScope` only
   for a valid saved/login session.
2. Logout, authorization rejection, account change, and server change advance the
   epoch and deconfigure or replace the old scope before old results can publish.
3. The worker opens a supported final DB or creates/promotes a fresh empty DB.
4. Network-available startup schedules/accepts Jellyfin reconciliation without
   blocking the UI.
5. Offline startup schedules DownloadStore-backed minimum hierarchy reconstruction
   when complete-download metadata is available.
6. Fresh/offline/partial state remains incomplete until Jellyfin reconciliation
   successfully commits the relevant generation.
7. Existing production legacy readers remain only as the pre-cutover compatibility
   path; CatalogDb never uses `catalog.v1` as an authority.
8. Expose safe nonblocking status for scope, bootstrap, reconciliation, and offline
   reconstruction decisions without private IDs, titles, tokens, or SQL.

## Invariants

- Normal app lifecycle is the activation path.
- No UI-thread DB/network work or waiting.
- SQLite cannot publish stale-scope results.
- Jellyfin and DownloadStore ownership remains separate.
- An offline first launch does not lose complete downloaded content.

## Focused tests

- Valid saved session opens/creates the current scope without legacy input.
- Valid login activates the same path.
- Existing final DB wins over stale temp state.
- No final DB creates fresh empty state.
- Online initial reconciliation populates and marks complete only after success.
- Offline first launch reconstructs complete-download hierarchy and stays incomplete.
- Logout/scope switch during bootstrap or reconciliation suppresses stale results.
- Reopen same scope returns the expected SQLite state.

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
feat(catalog): activate fresh database through app lifecycle
```

This task file authorizes exactly one commit for this task after successful validation.

## STOP conditions

Stop if normal login/saved-session flow cannot activate the path without blocking,
if offline downloads disappear, if a stale scope can publish, or if any path
requires reading or mutating `catalog.v1`.
