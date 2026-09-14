# Task 35 — Define durable incremental-sync checkpoints

**Phase:** E — incremental synchronization

## Objective

Establish the persisted checkpoint and state-transition contract used by
incremental library synchronization without changing the SQLite schema or
making the UI responsible for synchronization.

## Why

The existing timestamps describe successful catalog work, but incremental
catch-up needs an explicit rule for what may be queried after a clean exit,
network failure, reconnect, cancellation, or supersession.  A conservative
checkpoint must never advance past work that was not successfully applied.

## Preconditions

- CP-D is complete and the post-Task-33 repository is available.
- Original SQLite Task 34 remains PAUSED.

## Allowed Files

- `src/cache/SyncState.*`
- `src/library/LibrarySync.*`
- `src/catalog/CatalogDb.*` only if the existing worker-owned sync-state
  primitive must expose the already-supported checkpoint semantics
- `tests/cases/test_cache_offline.inc`
- `tests/cases/test_catalog_parity.inc`
- This task file

## Forbidden Scope

- Do not add schema v4 or new SQLite tables/columns.
- Do not add a second CatalogDb worker or SQLite connection.
- Do not implement Jellyfin WebSocket transport; that is Task 38.
- Do not fetch or delete artwork.
- Do not change Home presentation or transfer/playback behavior.

## Required behavior

1. A checkpoint advances only after the corresponding bounded catalog work
   has completed successfully and been published through LibrarySync.
2. Failed, cancelled, superseded, or partially applied work leaves the prior
   safe checkpoint intact.
3. A missing, malformed, clock-regressed, or otherwise uncertain checkpoint
   selects conservative catch-up/full-reconcile behavior.
4. The checkpoint remains scoped to the authenticated server/user catalog.
5. The contract uses the existing persistence mechanisms and remains readable
   by the current compatibility code.

## Method

1. Add focused tests for successful advancement, failure preservation,
   cancellation preservation, scope separation, and clock regression.
2. Run the focused cache/catalog test groups and verify the new assertions
   fail before the implementation change.
3. Implement only the state-transition correction needed by those tests.
4. Confirm all state I/O remains off the SDL/UI thread and worker-owned where
   SQLite is involved.

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

- The checkpoint requires a schema change, a second worker/connection, or a
  new legacy persistence path.
- Correctness requires editing a file outside **Allowed Files**.
- A failed/cancelled operation cannot preserve the prior safe checkpoint.
- Any SQLite, network, filesystem, retry, or blocking join moves to SDL/UI.

## Commit boundary

This task is exactly one commit after all validation passes.

```text
feat(sync): define incremental catalog checkpoints
```

Do not begin Task 36 until this task is committed.
