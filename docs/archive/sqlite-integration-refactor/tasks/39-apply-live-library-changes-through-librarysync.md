# Task 39 — Apply live library changes through LibrarySync

**Phase:** E — incremental synchronization

## Objective

Consume coalesced Jellyfin library events and route additions, updates, and
removals through LibrarySync and CatalogDb’s worker-owned primitives.

## Why

The event transport must not become a second synchronization owner.  Live
events are only an input; LibrarySync remains responsible for fetching the
affected metadata, applying membership changes, and preserving cancellation
and generation semantics.

## Preconditions

- Task 38 passes and is committed.

## Allowed Files

- `src/library/LibrarySync.*`
- `src/catalog/CatalogDb.*`
- `src/net/JellyfinApi.*`
- `src/net/JellyfinLibraryEvents.*`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_core.inc`
- `tests/cases/test_api_session.inc`
- This task file

## Forbidden Scope

- Do not let HomeScreen, DownloadManager, or JellyfinLibraryEvents write
  SQLite directly.
- Do not replace the existing full-generation safety path.
- Do not download artwork in LibrarySync.
- Do not introduce a generic repository, service locator, DI framework, or
  second SQLite worker/connection.

## Required behavior

1. Added/updated IDs fetch only the affected supported metadata where
   possible and apply it through LibrarySync.
2. Removed IDs are removed from current membership only through the
   worker-owned validated catalog operation.
3. Unknown or stale events are harmless and idempotent.
4. Event batches are bounded and superseded safely.
5. A fetch or persistence failure retains valid cached catalog data and marks
   catch-up/reconciliation as needed.
6. Changes to hierarchy items continue to use the existing LibrarySync
   hierarchy policy.

## Method

1. Add behavior tests for add, update, remove, duplicate events, stale events,
   mixed batches, failed fetch, cancellation, and supersession.
2. Verify the tests fail because the event transport currently has no catalog
   consumer.
3. Implement the narrow consumer path using existing worker/future patterns.
4. Verify no event callback performs synchronous network or SQLite work on the
   UI thread.

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

- Event application requires violating LibrarySync/CatalogDb ownership.
- A live update can publish partial membership or delete data after a
  transient failure.
- The affected hierarchy cannot preserve existing cancellation/generation
  behavior.
- Required production work falls outside **Allowed Files**.

## Commit boundary

```text
feat(sync): apply live library changes through librarysync
```

Do not begin Task 40 until this task is committed.
