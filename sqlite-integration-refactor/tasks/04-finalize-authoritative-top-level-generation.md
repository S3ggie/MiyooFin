# Task 04 — Finalize authoritative top-level generations atomically

**Phase:** A — correctness

## Objective

Publish a complete staged generation in one CatalogDb transaction and switch Home population to begin → stage → finalize/abort.

## Why

Warm databases retain stale views/memberships because bounded upserts never perform authoritative reconciliation.

## Preconditions

- Task 03 commit is present.
- Task 01 prevents optional rail errors from aborting catalog population.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreen.hpp` only for generation/cancellation state
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_migration.inc` if needed

## Forbidden Scope

- Do not delete stale `media_items`; Task 05 makes membership authoritative.
- Do not change schema version.
- Do not extract LibrarySync.
- Do not optimize startup latency.

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

1. Add a finalize command for the active top-level generation.
2. Finalize in one SQLite transaction: validate generation/scope, clear live membership/views, copy staged views in ordinal order, copy staged memberships, commit.
3. Any error/cancel/scope supersession before commit rolls back to the previous live membership set.
4. Successful finalize clears TEMP stage and active marker.
5. Home allocates one monotonic sync generation, begins before supported-view population, stages all pages, finalizes only after all supported views complete, and aborts on required failure/cancel.
6. Optional rail failure alone never aborts the generation.
7. On a fresh DB, do not publish staged membership as committed; remaining loading until finalize is acceptable in this correctness step.

## Tests

- Successful generation exactly replaces old live membership.
- Removed view/member disappears after finalize.
- Injected finalize failure preserves previous live rows.
- Mid-population cancel+abort preserves previous live rows.
- Home flow finalizes only after all supported views finish.

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
fix(catalog): publish complete top-level generations atomically
```

Do not include any part of Task 05 in this commit.
