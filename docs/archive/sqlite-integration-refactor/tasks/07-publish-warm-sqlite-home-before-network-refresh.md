# Task 07 — Publish warm SQLite Home before network refresh

**Phase:** A — correctness

## Objective

Make an existing committed SQLite catalog usable after scope activation before any network refresh completes.

## Why

Persistent SQLite currently provides no warm-start advantage because Home waits on serial network work plus a page commit.

## Preconditions

- Tasks 04-06 define committed membership and ephemeral rails.
- App configures scope before Home construction.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/HomeTabs.cpp` and `.hpp` if a tab-shell helper is needed
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_ui_foundation.inc`

## Forbidden Scope

- Do not block App/SDL waiting for scope readiness.
- Do not read a full LibrarySnapshot.
- Do not wait for rails/network before warm publication.
- Do not extract LibrarySync/LibraryQuery yet.
- Do not change durability settings.

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

1. Add a bounded CatalogDb query for committed view descriptors: id/name/collection type/ordinal only.
2. From an existing Home background path, enqueue committed-view plus first bounded movie/show reads immediately; CatalogDb's earlier scope command must execute first on its worker.
3. If committed supported membership exists, publish Home Ready from SQLite before CW/RA/Views/network pages.
4. Build tab shells from descriptors and only bounded first-page windows.
5. Continue network refresh in background; failure preserves displayed committed catalog.
6. A genuinely empty/fresh catalog remains loading until its first complete authoritative generation commits.
7. Preserve selected tab/item when a later committed generation refreshes membership where possible.

## Tests

- Warm catalog publishes content with network forced unavailable.
- Warm publication never calls LibraryCache or full snapshot read.
- Fresh empty catalog is not falsely published as authoritative.
- Network failure after warm publication keeps content visible.
- No SQLite operation occurs on the test caller representing UI thread.

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
feat(home): publish warm sqlite catalog first
```

Do not include any part of Task 08 in this commit.
