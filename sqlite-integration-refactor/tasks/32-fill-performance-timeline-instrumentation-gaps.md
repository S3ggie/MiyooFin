# Task 32 — Fill performance timeline instrumentation gaps

**Phase:** D — performance rebaseline

## Objective

Ensure telemetry can measure each required warm/cold Home stage; add only missing low-overhead markers before the final hardware rebaseline.

## Why

The old timing could not isolate rail requests, route fallback, page network time, DB queue/transaction/commit, read-page cost, or artwork contention.

## Preconditions

- CP-C is explicitly approved.
- Task 31 hardware correctness passes.

## Allowed Files

- `src/diagnostics/PerformanceTelemetry.*`
- `src/diagnostics/TelemetryTypes.hpp` and `TelemetryIds.hpp` only if existing v2 needs fields/records
- `src/diagnostics/TelemetryGuards.hpp`
- `src/app/UiDiagnostics.*` for one-shot text stages
- `src/net/RouteRequest.hpp` and `src/net/HttpClient.*` only for timing route attempts without routing changes
- `src/catalog/CatalogDb.cpp` only for missing DB timing markers
- `src/library/LibrarySync.*`
- `src/library/LibraryQuery.*`
- `src/ui/screens/HomeScreen*.cpp` only for first-use markers
- `tests/cases/test_telemetry.inc`
- `tests/test_telemetry_decoder.py` if binary telemetry changes
- `telemetry/SCHEMA_V2.md` only if v2 changes

## Forbidden Scope

- Do not optimize behavior.
- Do not change request order/timeouts/route policy/PRAGMAs/journal/synchronous/page sizes/thread counts/artwork policy/UI.
- Do not alter frozen MFT v1.
- Do not log tokens/full authenticated URLs.

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

1. Inventory existing telemetry first and reuse it.
2. Required measurable stages: scope request/ready; warm committed query request/ready; first useful Home frame; CW/RA/Views request durations; each route attempt/fallback; first library page HTTP; CatalogDb enqueue/dequeue wait; page transaction and commit; readMediaPage; artwork-worker overlap; RSS/peak, CPU, telemetry drops/writer errors.
3. Add only missing markers/fields; prefer UiDiagnostics for one-shot ordering and PerformanceTelemetry for numeric duration/counters.
4. Use monotonic clocks.
5. Telemetry calls must remain nonblocking through existing writer/ring model.
6. Update decoder/schema docs only when binary output changes.

## Tests

- New event/field encode/decode test if added.
- No credentials/authenticated URLs in route telemetry.
- v1 fixtures remain unchanged.
- Telemetry-disabled behavior remains valid where covered.

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

No device run in this task. ARM build/verify is mandatory; Task 33 does device measurement.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
perf(telemetry): cover library startup timeline
```

Do not include any part of Task 33 in this commit.
