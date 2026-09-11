# Task 02 — Add begin/abort top-level sync staging

**Phase:** A — correctness

## Objective

Add worker-owned temporary staging state and explicit begin/abort commands for one top-level library synchronization generation without changing live committed membership.

## Why

Page writes currently mutate live membership incrementally. Failed or cancelled refreshes need disposable state that leaves the last valid generation intact.

## Preconditions

- Task 01 commit is present.
- Schema v3 opens successfully with the existing bootstrap path.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `tests/cases/test_catalog_migration.inc` or `tests/cases/test_catalog_parity.inc`
- `tests/test_main.cpp` only if required

## Forbidden Scope

- Do not change persistent schema version or `CatalogDbSchema.hpp`.
- Do not switch page writes to staging yet.
- Do not change HomeScreen/network orchestration.
- Do not add another SQLite connection/worker.

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

1. Add a top-level sync generation identifier distinct from scope epoch and existing stale-work metadata.
2. On the CatalogDb worker, begin creates/reuses TEMP staging tables for library views and memberships, clears abandoned stage rows, and marks exactly one active generation.
3. TEMP staging must disappear on process restart and require no schema-version bump.
4. Begin validates current scope and never mutates live `library_views`/`library_membership`.
5. Abort is idempotent for the active generation: clear staging rows/active marker and leave live rows untouched.
6. Beginning a newer generation discards old staging before activating the new generation.
7. Use futures/results consistent with existing CatalogDb command patterns.

## Tests

- Seed live rows, begin a new generation, assert live rows unchanged.
- Begin then abort leaves live rows unchanged.
- Stale scope epoch cannot begin a stage.
- Newer generation leaves no staged rows from the older generation.

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
refactor(catalog): add top-level sync staging lifecycle
```

Do not include any part of Task 03 in this commit.
