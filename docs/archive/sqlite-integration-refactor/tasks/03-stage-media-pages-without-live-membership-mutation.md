# Task 03 — Stage top-level media pages without live membership mutation

**Phase:** A — correctness

## Objective

Teach bounded media-page writes to stage view/membership rows for an active top-level sync generation while continuing to upsert canonical media metadata transactionally.

## Why

Current page writes expose partial generations by directly mutating live views/membership.

## Preconditions

- Task 02 begin/abort staging is present and tested.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_migration.inc` if rollback fixtures live there
- `tests/test_main.cpp` only if needed

## Forbidden Scope

- Do not switch Home runtime to staged generations yet; Task 04 does that.
- Do not finalize/sweep live membership.
- Do not add persistent generation columns/schema v4.
- Do not change read queries.

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

1. Extend the bounded page-write request with an explicit top-level sync generation for staged authoritative population.
2. For a valid active generation, upsert media scalars/genres/image tags/sort key as today, but write view/member rows into TEMP staging instead of live membership.
3. Keep one bounded transaction per submitted page; page failure rolls back both metadata and staging writes from that page.
4. Reject a staged page with inactive generation or stale scope.
5. Retain the old non-staged page path only as compatibility/test behavior until Task 04 runtime cutover; mark it compatibility-only.
6. Do not delete live views/memberships while staging.

## Tests

- Staged page updates media metadata but not live membership.
- Two staged pages preserve deterministic member ordinals.
- Injected page failure rolls back page media and staging rows.
- Page for stale/aborted generation is rejected.

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
refactor(catalog): stage bounded library pages
```

Do not include any part of Task 04 in this commit.
