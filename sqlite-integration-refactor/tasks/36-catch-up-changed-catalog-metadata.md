# Task 36 — Catch up changed catalog metadata after downtime

**Phase:** E — incremental synchronization

## Objective

Use bounded timestamp-based Jellyfin item queries to catch up additions and
metadata updates since the last safe checkpoint after MiyooFin was offline.

## Why

Jellyfin does not expose a durable replayable event log for disconnected
clients, but its item queries can identify recently saved metadata.  This is
the fast restart path; it is not sufficient by itself to discover deletions.

## Preconditions

- Task 35 passes and is committed.
- Existing `getChangedHierarchyItems()` behavior and its cancellation rules
  remain intact.

## Allowed Files

- `src/net/JellyfinApi.*`
- `src/net/JellyfinApiHierarchy.cpp` only when extending the existing bounded
  changed-item query without changing its hierarchy contract
- `src/library/LibrarySync.*`
- `src/catalog/CatalogDb.*` only for existing worker-owned bounded item
  persistence primitives required by the catch-up path
- `tests/cases/test_api_session.inc`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_cache_offline.inc`
- This task file

## Forbidden Scope

- Do not claim this path detects deletions; deletion handling is Task 37.
- Do not materialize the entire library in RAM.
- Do not download artwork as part of metadata catch-up.
- Do not add WebSocket transport; that is Task 38.
- Do not bypass LibrarySync or write SQLite from JellyfinApi/HomeScreen.

## Required behavior

1. The query is paginated and cancellation-aware.
2. It returns only the supported catalog domains needed for Movies and
   Shows/Anime metadata catch-up.
3. The lower-bound timestamp is applied conservatively so changes at the
   boundary are not skipped.
4. Changed items are fetched/applied through LibrarySync and CatalogDb’s
   existing worker boundary.
5. Empty results are a successful no-op, not a catalog-clearing operation.
6. HTTP failure, cancellation, and malformed responses preserve the prior
   committed catalog and checkpoint.

## Method

1. Add API tests for pagination, boundary timestamps, empty results,
   cancellation, malformed data, and transient HTTP failure.
2. Add a LibrarySync/catalog regression proving changed metadata updates the
   cached item without replacing unrelated catalog rows.
3. Verify the tests fail for the missing catch-up behavior.
4. Implement the smallest bounded query and orchestration path.
5. Confirm artwork remains an independent ImageCache concern.

## Validation

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
git diff --check
```

## STOP conditions

- The Jellyfin API cannot provide a bounded, cancellation-aware query without
  unbounded response materialization.
- The implementation would silently treat “no changed rows” as proof that
  no deletions occurred.
- Required production work falls outside **Allowed Files**.
- A second network/SQLite worker or UI-thread blocking operation is needed.

## Commit boundary

```text
feat(sync): catch up changed catalog metadata
```

Do not begin Task 37 until this task is committed.
