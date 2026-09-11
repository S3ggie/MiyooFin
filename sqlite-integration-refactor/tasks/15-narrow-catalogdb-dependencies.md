# Task 15 — Narrow CatalogDb to SQLite persistence/query execution

**Phase:** B — ownership/modularity

## Objective

Remove network/UI/download/cache orchestration dependencies from CatalogDb while preserving worker/connection/schema/migrations/transactions/bounded primitives.

## Why

CatalogDb currently reaches upward into JellyfinApi, DownloadStore, LibraryCache, UI sorting, and app diagnostics.

## Preconditions

- Tasks 10-14 moved synchronization/query/compatibility concerns outward.

## Allowed Files

- `src/catalog/CatalogDb.cpp`
- `src/catalog/CatalogDb.hpp`
- `src/catalog/MediaItemSql.*`
- Create one small pure helper under `src/catalog/` or `src/data/` if shared sort/scope identity logic must move down
- `src/ui/TitleOrganization.hpp` only to redirect shared pure logic
- `src/cache/LibraryCache.*` only to redirect shared scope-key logic
- `src/library/*` for adapting moved orchestration
- `Makefile`
- `Makefile.cross`
- `tests/cases/test_catalog_migration.inc`
- `tests/cases/test_catalog_parity.inc`

## Forbidden Scope

- Do not split CatalogDb into interfaces/classes.
- Do not change worker/connection count.
- Do not change PRAGMAs/durability/bootstrap.
- Do not touch download transfer/persistence or ImageCache.
- Do not remove telemetry.

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

1. CatalogDb may depend on lower data/catalog helpers, SQLite, and generic diagnostics; it must not include/call Jellyfin network APIs, UI presentation helpers, DownloadStore, or LibraryCache snapshot APIs.
2. Move organizational sort-key logic needed by SQL below UI and share it back upward.
3. Move server/user scope-key derivation to a neutral lower helper if CatalogDb still needs LibraryCache for it.
4. Offline reconstruction that reads DownloadStore belongs outside CatalogDb; retain only bounded metadata primitives.
5. Keep useful SQLite test/diagnostic methods.
6. Run `make refactor-check`.

## Tests

- Source dependency check finds no JellyfinApi/DownloadStore/LibraryCache/ui includes in CatalogDb.
- Scope-key compatibility is unchanged.
- Organizational sort ordering is unchanged.
- Schema/bootstrap/query tests remain passing.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
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
refactor(catalog): narrow persistence dependencies
```

Do not include any part of Task 16 in this commit.
