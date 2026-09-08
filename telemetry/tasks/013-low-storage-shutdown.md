# Task 013 — enforce low-storage trace shutdown

## Status
NOT STARTED

## Depends On
- `012`

## Goal
Disable telemetry trace writing before it competes with application/download storage.

## Why This Task Exists
Long sessions need a concrete low-space safety policy, not a future concept.

## Allowed Files
- `src/diagnostics/TelemetryConfig.hpp`
- `src/diagnostics/TelemetryConfig.cpp`
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `src/diagnostics/TelemetryWriter.hpp`
- `src/diagnostics/TelemetryWriter.cpp`
- `src/diagnostics/LinuxProcessMetrics.hpp`
- `src/diagnostics/LinuxProcessMetrics.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not delete application/download data.
- Do not auto-reenable in same process.
- Do not lower override clamp below 64MiB.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Use default minFreeStorageBytes=128MiB and existing 64–4096MiB override clamp.
2. Before opening first trace, sample free storage. If valid and below threshold, create no trace, print one safe stderr notice, set enabled false, and let service thread exit.
3. During active tracing evaluate free space on the existing 10s cadence.
4. On first valid crossing below threshold directly append SessionEvent WriterDisabledLowSpace with value0 threshold MiB and value1 observed free bytes.
5. Directly append one final TelemetryHealth.
6. Flush/close, set runtime enabled false, stop periodic work, let service thread exit, and never auto-reenable.
7. Invalid free-space sample does not disable; retry later.
8. Add tests for startup-below-threshold no-file and active crossing final-event/no-growth.

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
- 128MiB default enforced.
- Low-space terminal event/health written when possible.
- No trace growth after shutdown.

## Commit
```sh
git add src/diagnostics/TelemetryConfig.hpp src/diagnostics/TelemetryConfig.cpp src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp src/diagnostics/TelemetryWriter.hpp src/diagnostics/TelemetryWriter.cpp src/diagnostics/LinuxProcessMetrics.hpp src/diagnostics/LinuxProcessMetrics.cpp tests/cases/test_telemetry.inc
git commit -m "feat: stop telemetry on low storage"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
