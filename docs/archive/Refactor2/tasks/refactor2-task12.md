# Refactor2 Task 12 — Modularize performance telemetry

## Objective

Split `PerformanceTelemetry.cpp` into recording/state operations, service/rotation lifecycle, and snapshot/summary operations while keeping one `PerformanceTelemetry` owner and telemetry schema unchanged.

## Preconditions

- Task 11 commit exists.
- All focused telemetry binaries from Task 1 pass.

## Allowed Files

- `src/diagnostics/PerformanceTelemetry.cpp`, `PerformanceTelemetry.hpp`
- New `src/diagnostics/PerformanceTelemetryRecord.cpp`
- New `src/diagnostics/PerformanceTelemetryService.cpp`
- New `src/diagnostics/PerformanceTelemetrySnapshot.cpp`
- Existing diagnostics private headers or one new `PerformanceTelemetryInternal.hpp`
- Telemetry test files
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- `docs/architecture.md`, `docs/performance-telemetry.md`

## Required result

- Move high-frequency set/record/counter definitions to `PerformanceTelemetryRecord.cpp`.
- Move start/stop, service thread, sampling, flush, file rotation, and exclusive helpers to `PerformanceTelemetryService.cpp`.
- Move snapshot/accessor/summary definitions to `PerformanceTelemetrySnapshot.cpp`.
- Keep schema IDs, record bytes, atomics, memory ordering, fast-path behavior, sampling intervals, queue limits, rotation policy, test hooks, thread ownership, and noexcept contracts unchanged.
- Do not merge `TelemetryWriter`, `MftFormat`, or telemetry ID/type headers.
- Update docs only to reflect file ownership.

## Verification

Run every telemetry binary from Task 1 and the decoder test, then:

```sh
python3 tests/test_telemetry_decoder.py
make test -j2
make -j2
git diff --check
```

STOP if a schema/record change or new synchronization strategy is required.

## Commit

```text
refactor(telemetry): split recorder and service units
```
