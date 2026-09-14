# Task 01 — Isolate optional Home rail failures

**Phase:** A — correctness

## Objective

Make Continue Watching and Recently Added failures independent from top-level library population so either optional rail can fail without truncating Movies/Shows synchronization.

## Why

The audit found one failure flag is shared by optional rail requests and library-page failures; the outer view loop can break after a rail failure.

## Preconditions

- Planning baseline commit is present.
- Use current audited Home sync behavior; do not extract new owners yet.

## Allowed Files

- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreen.hpp` only if a narrow status field is required
- `tests/cases/test_catalog_parity.inc` or `tests/cases/test_api_session.inc`
- `tests/test_main.cpp` only if needed by the harness

## Forbidden Scope

- Do not change CatalogDb schema/membership semantics.
- Do not extract `LibrarySync`.
- Do not change request order, route fallback, HTTP timeouts, page sizes, layout, artwork, downloads, or playback.

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

1. Use separate status for optional Home rails versus required catalog population.
2. A Continue Watching failure records/logs the rail failure and still runs Recently Added, Views, and every supported library view.
3. A Recently Added failure records/logs the rail failure and still runs Views and every supported library view.
4. A Views failure or required library page/write failure remains a catalog-refresh failure.
5. Telemetry/status must not report a successful committed catalog as failed solely because an optional rail is unavailable.

## Tests

- Continue Watching failure with two supported views still processes both views.
- Recently Added failure with two supported views still processes both views.
- A real library-page failure still stops/marks catalog population failed.

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
fix(home): isolate optional rail failures
```

Do not include any part of Task 02 in this commit.
