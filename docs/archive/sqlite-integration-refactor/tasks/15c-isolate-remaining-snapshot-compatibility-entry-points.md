# Task 15C — Isolate remaining snapshot compatibility entry points

**Phase:** B — ownership/modularity

## Objective

Make legacy `LibrarySnapshot` seed/read conversion explicitly compatibility/test-only while preserving legacy files and keeping all SQLite execution on the existing `CatalogDb` worker.

Task 13 already introduced `CatalogCompatibility`; Task 15C builds on that seam instead of introducing another abstraction.

## Why

`LibrarySnapshot` exists for legacy compatibility and tests, but normal runtime catalog synchronization/query paths should not expose or depend on whole-snapshot APIs.

The final ownership is:

- `CatalogCompatibility` owns the `LibrarySnapshot`-facing compatibility API/conversion boundary.
- `CatalogDb` owns SQLite execution only.
- Normal runtime consumers use bounded/domain APIs.
- Legacy cache readers/files remain intact until original Task 34 is explicitly authorized.

## Preconditions

- Task 13 compatibility bridge exists.
- Task 15A is complete.
- Task 15B is complete.
- Original SQLite Task 34 remains PAUSED.

## Allowed Files

- `src/catalog/CatalogDb.cpp`
- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogCompatibility.hpp`
- `src/catalog/CatalogCompatibility.cpp`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_migration.inc`
- `tests/cases/test_cache_offline.inc`
- `Makefile`
- `Makefile.cross`
- this Task 15C roadmap file only for restoring the missing committed definition

Allowed Files are a hard boundary.

## Forbidden Scope

Do not:

- delete `LibraryCache` files;
- delete or rewrite user legacy cache files;
- implement original Task 34;
- change schema v3;
- add another SQLite connection or worker;
- expose `LibrarySnapshot` through `LibrarySync`, `LibraryQuery`, `OfflineLibraryQuery`, Home, Series, Episode, or DownloadManager;
- move SQLite execution outside `CatalogDb`;
- modify playback;
- begin final Task 15 or Task 16.

## Architecture invariants

Preserve:

- schema v3;
- exactly one `CatalogDb` SQLite worker;
- exactly one SQLite connection for active scope;
- bounded queues;
- bounded/keyset reads;
- no full-library RAM materialization in normal runtime flows;
- no SQLite/network/long filesystem/retry sleep/blocking joins on the SDL/UI thread;
- atomic fresh-database bootstrap/promotion behavior;
- `DownloadStore` as physical offline-availability authority;
- `ImageCache` as artwork-byte cache;
- no production dual-write to legacy whole-file persistence.

## Implementation requirements

1. `CatalogCompatibility` is the only public compatibility-facing owner of:
   - `seedLibrarySnapshot`;
   - `seedLibrarySnapshotForTest`;
   - `readLibrarySnapshot`.
2. `CatalogDb.hpp` must not expose normal public `LibrarySnapshot` seed/read APIs.
3. `CatalogDb` may retain private/internal command structs and processing methods required to execute compatibility SQL on its existing worker.
4. Compatibility helper calls must still enqueue onto the existing `CatalogDb` worker/connection.
5. Normal runtime consumers must not call snapshot seed/read APIs.
6. Legacy `LibraryCache` readers and files remain intact and untouched.

## Tests

- Compatibility seed/read fixtures still round-trip through the isolated bridge.
- Seed idempotency, rollback, stale-scope rejection, and top-level membership parity remain passing.
- Normal runtime source files do not call snapshot seed/read APIs.
- `CatalogDb` retains one worker-owned SQLite connection and schema-v3 behavior.
- Offline/cache compatibility tests remain passing.

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
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL/UI thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
refactor(catalog): isolate snapshot compatibility entry points
```

Do not include final Task 15 or Task 16 in this commit.
