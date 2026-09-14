# Task 09 — Build offline root from DownloadStore plus catalog metadata

**Phase:** A — correctness

## Objective

Build offline Movies/Shows roots from DownloadStore availability, enriched by CatalogDb metadata with DownloadItem fallback, without mandatory LibrarySnapshot.

## Why

Offline root currently requires legacy cache and can later page online-only catalog rows, while deeper screens use downloaded-only logic.

## Preconditions

- Task 08 establishes stable scope ownership.
- Task 05 provides membership-authoritative online reads.

## Allowed Files

- Create `src/library/OfflineLibraryQuery.hpp`
- Create `src/library/OfflineLibraryQuery.cpp`
- `src/catalog/CatalogDb.hpp` and `.cpp` only for a bounded metadata-by-ID primitive
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreen.cpp` and `.hpp` only for offline result consumption
- `src/cache/OfflineLibraryProjection.*` only to move/reuse synthesis logic
- `Makefile`
- `Makefile.cross`
- `tests/cases/test_cache_offline.inc`
- `tests/cases/test_catalog_parity.inc`

## Forbidden Scope

- Do not change download transfer/HLS/retry/manifests/local playback.
- CatalogDb must not decide byte availability.
- Do not require LibraryCache/OfflineCatalog/LibrarySnapshot for offline root.
- Do not load full online catalog into RAM.

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

1. Create one concrete `OfflineLibraryQuery`; no interface hierarchy.
2. Availability input is a DownloadSnapshot derived from DownloadStore and uses existing complete/local-only/update-available semantics.
3. Collect downloaded movie IDs and series IDs represented by downloaded episodes; query CatalogDb metadata in batches of at most 64 IDs.
4. Use canonical title/genres/image tags/hierarchy IDs when found; synthesize minimum Movie/Series metadata from DownloadItem when absent.
5. Offline root contains only physically available downloads.
6. Manual-offline Home starts successfully without old LibraryCache.
7. Keep Series/Episode downloaded-only behavior and playback unchanged.

## Tests

- Downloaded movie uses canonical metadata when present.
- Missing catalog metadata falls back to DownloadItem.
- Downloaded episodes produce one root series.
- Online-only catalog item is absent offline.
- No LibraryCache file is required.
- By-ID query remains bounded to <=64 rows per command.

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
fix(offline): build roots from downloads and catalog
```

Do not include any part of Task 10 in this commit.

## Checkpoint

**CP-A — Correctness foundation. STOP after this task and obtain explicit approval before Phase B.**

Do not start the next phase until this checkpoint is explicitly approved.
