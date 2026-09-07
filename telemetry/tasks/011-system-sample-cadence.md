# Task 011 — emit periodic SystemSample records

## Status
NOT STARTED

## Depends On
- `010`

## Goal
Schedule process metrics entirely on the service thread.

## Why This Task Exists
System metrics must share monotonic trace time without touching SDL or worker locks.

## Allowed Files
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `src/diagnostics/LinuxProcessMetrics.hpp`
- `src/diagnostics/LinuxProcessMetrics.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No application locks.
- No catch-up burst.
- No CPU-percent calculation on device.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Construct LinuxProcessMetrics on service thread.
2. Schedule base samples every sampleIntervalMs using monotonic deadlines.
3. Refresh free space only every freeSpaceIntervalMs; carry last valid free bytes plus sample age between refreshes.
4. Emit SystemSample directly from service thread with cumulative metrics, logical trace bytes, drops, age, validity flags.
5. If more than one interval late, increment sampling_late once and reschedule from now; do not emit catch-up bursts.
6. While sampling suspended, keep draining ring but skip periodic samples and reset deadline on resume.
7. Use injectable fake metric/time hooks in tests.

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
- System cadence deterministic.
- Suspension has no backfill.
- No worker mutex acquired.

## Commit
```sh
git add src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp src/diagnostics/LinuxProcessMetrics.hpp src/diagnostics/LinuxProcessMetrics.cpp tests/cases/test_telemetry.inc
git commit -m "feat: emit system samples"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
