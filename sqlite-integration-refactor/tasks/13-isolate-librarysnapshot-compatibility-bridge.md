# Task 13 — Isolate LibrarySnapshot compatibility bridge

**Phase:** B — ownership/modularity

## Objective

Remove LibrarySnapshot from normal CatalogDb runtime APIs and isolate seed/read conversion behind compatibility/test code.

## Why

CatalogDb public API imports LibraryCache and preserves full-snapshot APIs that SQLite paging was meant to retire.

## Preconditions

- Task 12 normal Home no longer needs snapshot APIs.
- Task 09 offline root no longer requires LibrarySnapshot.

## Allowed Files

- Create `src/catalog/CatalogCompatibility.hpp`
- Create `src/catalog/CatalogCompatibility.cpp`
- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `src/cache/LibraryCache.*` only for compatibility relocation
- `Makefile`
- `Makefile.cross`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_cache_offline.inc`
- `tests/cases/test_catalog_migration.inc`

## Forbidden Scope

- Do not delete LibraryCache files/user legacy files; original Task 34 remains paused.
- Do not remove required migration/parity tests.
- Do not reintroduce snapshots through LibrarySync/Query.
- Do not change schema v3.

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

1. Move full-snapshot seed/read conversion out of normal CatalogDb public API into clearly named compatibility helper.
2. `CatalogDb.hpp` no longer includes LibraryCache merely for runtime method/result types.
3. Normal App/Home/Series/Episode/DownloadManager/LibrarySync/LibraryQuery/OfflineLibraryQuery do not call snapshot seed/read APIs.
4. Compatibility helper must still use the one CatalogDb worker/connection, not open SQLite.
5. Legacy files remain untouched on disk.

## Tests

- Snapshot compatibility fixture still round-trips via isolated bridge.
- Normal runtime headers no longer require LibrarySnapshot through CatalogDb.
- Offline-without-LibraryCache test remains passing.

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
refactor(catalog): isolate snapshot compatibility
```

Do not include any part of Task 14 in this commit.
