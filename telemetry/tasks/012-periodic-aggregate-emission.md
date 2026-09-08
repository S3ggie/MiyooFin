# Task 012 — establish periodic Worker Artwork Download and Health aggregates

## Status
NOT STARTED

## Depends On
- `011`

## Goal
Make the service thread the explicit owner of WorkerSample, ArtworkSummary, DownloadSample, and TelemetryHealth emission.

## Why This Task Exists
These aggregate records need defined cadence/reset semantics before producer tasks feed them.

## Allowed Files
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not instrument concrete workers/cache/decoder/download code yet.
- Do not acquire application locks.
- Do not emit aggregates from producers.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add one fixed worker slot per WorkerId with current active/depth, interval highwater, interval completed/failed/cancelled counters.
2. Add atomic producer APIs to update worker state/counters only.
3. Every base 1000ms tick emit one WorkerSample for every WorkerId. Snapshot current active/depth; exchange deltas to zero; reset highwater to at least current depth.
4. Add ArtworkSummary interval counters. Producer APIs are reserved for ImageCache/ImageDecoder only.
5. Every tick emit one ArtworkSummary; exchange interval counts/bytes/total to zero and reset decode max.
6. Add DownloadSample current gauges plus interval bytes/segments/retries; add producer APIs for DownloadManager/HLS.
7. Every tick emit one DownloadSample; current gauges persist, interval deltas reset, bytes_per_sec uses bytes_delta and actual interval_us.
8. Every tick emit TelemetryHealth directly. Queue depth/buffered bytes are current, queue highwater resets each interval, drops/errors/rotations/lateness remain cumulative.
9. All four aggregate record types are emitted directly by the service thread, not through the producer ring.
10. Add two-interval tests proving reset/persistence semantics.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- All four required aggregates have explicit service cadence.
- Delta/reset semantics match SCHEMA_V1.md.
- Later DownloadManager task need not modify PerformanceTelemetry.cpp.

## Commit
```sh
git add src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp tests/cases/test_telemetry.inc
git commit -m "feat: add periodic telemetry aggregates"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
