# Task 13 — SQLite telemetry with deliberate MFT v2

## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- In autonomous roadmap mode, after successful validation and the task commit, immediately open the next numbered task and continue without asking for routine confirmation.

## Goal

Add CatalogDb-specific performance telemetry without changing or reinterpreting frozen MFT v1. Introduce a deliberate MFT v2 that is a strict, decoder-compatible extension.

## Depends On

- Task 12

## Allowed Files

- `src/diagnostics/PerformanceTelemetry.*`
- `src/diagnostics/MftFormat.*`
- `src/diagnostics/TelemetryIds.hpp` / telemetry type files as narrowly required
- CatalogDb telemetry producer integration under `src/catalog/`
- New `telemetry/SCHEMA_V2.md`
- Telemetry decoder/analyzer tools under `tools/` and telemetry tests under `tests/`
- Build/test source lists only if a new telemetry translation unit is necessary

## Forbidden Scope

- **Do not modify `telemetry/SCHEMA_V1.md`. MFT v1 is frozen.**
- Do not change byte layouts, enum meanings, record sizes, or semantics of any v1 trace.
- Do not reinterpret an existing v1 WorkerId/record type to mean CatalogDb.
- No media IDs, titles, URLs, DB paths, SQL text, scope keys, or credentials in traces.
- No telemetry file-location/rotation ownership changes.
- No blocking telemetry calls from CatalogDb.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Read frozen `telemetry/SCHEMA_V1.md` and current v1 encoder/decoder tests.
- Confirm existing v1 exact `WorkerId`, `worker_mask`, and record-type enums do not contain a semantically correct CatalogDb identity.
- Treat that absence as the reason a new fixed record/enum extension requires MFT v2 rather than semantic overloading.

## Exact implementation requirements

1. Create **`telemetry/SCHEMA_V2.md`**. Do not edit v1.
2. MFT v2 file header uses `schema_version=2`.
3. Every existing v1 record type 1–14 retains its existing binary layout and meaning in v2 unless `SCHEMA_V2.md` explicitly copies it unchanged. Existing v1 traces must remain decodable byte-for-byte.
4. Extend v2 WorkerId with `CatalogDb` and assign a new nonconflicting value; extend the v2 UI-stall worker mask using a previously reserved bit only if needed and document it.
5. Add one fixed-layout v2-only `CatalogDbSummary` record type for interval metrics. Its contract must include:
   - query count, total µs, max µs;
   - transaction count, total µs, max µs;
   - commit count, total µs, max µs;
   - queue-wait total µs and max µs;
   - enqueue-rejected delta;
   - rows inserted/updated/deleted;
   - SQLITE_BUSY-family delta;
   - SQLITE_IOERR-family delta;
   - SQLITE_CORRUPT/NOTADB delta;
   - reserved zero fields for forward compatibility.
6. Queue depth/high-water/active/completed/failed/cancelled should use the new **v2 CatalogDb WorkerSample identity**, not be duplicated unnecessarily in the summary.
7. All CatalogDb timing uses the existing monotonic telemetry clock.
8. CatalogDb producers update fixed numeric counters/gauges only; the telemetry service thread performs periodic serialization exactly like existing aggregate ownership.
9. Decoder/analyzer must accept both schema versions:
   - v1: existing exact behavior;
   - v2: decode unchanged v1-compatible record layouts plus the new v2 CatalogDb additions.
10. Add golden/fixture tests proving pre-existing MFT v1 trace bytes still decode successfully with the revised decoder.
11. Add v2 round-trip/layout tests and corruption/unknown-record forward-compatibility tests.
12. Runtime-off and compile-out behavior must remain cheap and must not create telemetry work/files when disabled.
13. Benchmark-only DB/WAL/journal file sizes stay outside MFT and are collected by benchmark tooling; do not add filesystem paths to the trace.

## Invariants

- MFT v1 contract is unchanged.
- No semantic abuse of v1 enums/records.
- v2 is explicit and versioned.
- Old v1 traces remain supported by decoder/analyzer.
- CatalogDb telemetry is numeric, fixed-layout, bounded, nonblocking, and privacy-safe.

## Focused tests

- Byte-for-byte v1 fixture still decodes under updated tooling.
- v1 decoder path rejects impossible v2-only semantics rather than mislabeling them.
- v2 header/version decode.
- v2 CatalogDb WorkerSample identity.
- v2 CatalogDbSummary exact layout/round-trip.
- Query/transaction/commit/queue-wait aggregation.
- SQLite error-family counters.
- Telemetry-disabled path.
- Privacy scan finds no test IDs/titles/paths/SQL strings in generated trace.

## Complete validation commands

```sh
make test -j2
make -j2
python3 tests/test_telemetry_decoder.py
# Run any new v1 golden-fixture and v2 decoder/analyzer tests added by this task
git diff --check
git status --short
```

## Commit message

```text
feat(telemetry): introduce MFT v2 CatalogDb metrics
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Supporting CatalogDb metrics would require modifying `telemetry/SCHEMA_V1.md`.
- The decoder cannot support existing v1 and new v2 traces concurrently.
- The proposed v2 record leaks IDs/paths/SQL text or blocks CatalogDb.
- A fixed numeric summary cannot represent the required metrics without unbounded data.

Do not broaden the task to work around a STOP condition.
