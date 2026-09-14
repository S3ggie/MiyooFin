# Task 37 — Reconcile authoritative membership and deletions

**Phase:** E — incremental synchronization

## Objective

Add a bounded authoritative membership check that removes catalog items no
longer present on Jellyfin while preserving the last committed generation on
failure or cancellation.

## Why

Changed-since queries cannot return an item that was deleted.  Deletion
correctness therefore needs an authoritative membership comparison, separate
from metadata catch-up.

## Preconditions

- Task 36 passes and is committed.
- Existing top-level temporary staging/finalization behavior is preserved.

## Allowed Files

- `src/net/JellyfinApi.*`
- `src/library/LibrarySync.*`
- `src/library/LibraryQuery.*` only for a bounded current-membership read
- `src/catalog/CatalogDb.*`
- `tests/cases/test_catalog_core.inc`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_cache_offline.inc`
- This task file

## Forbidden Scope

- Do not weaken hierarchy consistency validation.
- Do not delete catalog rows merely because a transient request failed.
- Do not materialize the full metadata library in RAM; use bounded pages,
  keyset cursors, or worker-owned staging.
- Do not touch DownloadStore’s physical availability authority.
- Do not download artwork or change Home rendering.

## Required behavior

1. Server membership is enumerated in bounded pages for each supported view.
2. Items absent from the authoritative server membership disappear from live
   CatalogDb membership only after a successful complete reconciliation.
3. A missing view, partial response, HTTP failure, cancellation, or
   supersession preserves the prior committed generation.
4. Moves between supported libraries remain represented exactly once per
   current membership rules.
5. The operation remains owned by LibrarySync and executes persistence on the
   CatalogDb worker.

## Method

1. Add regressions for one removed movie, one removed series, a deleted view,
   a moved item, mid-page cancellation, and a failed finalization.
2. Prove the tests fail because the existing changed-item path cannot account
   for removals.
3. Reuse the existing generation staging/finalization primitives whenever
   possible; add only the missing bounded membership operation.
4. Confirm local cached content remains readable until a valid replacement
   generation is published.

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

- Deletion correctness requires weakening CatalogDb validation or changing
  schema v3.
- A complete membership pass requires full-library RAM materialization.
- Failure/cancellation can expose a partial generation.
- Required production work falls outside **Allowed Files**.

## Commit boundary

```text
fix(sync): reconcile deleted catalog membership
```

Do not begin Task 38 until this task is committed.
