# Task 010 — implement Linux process metrics

## Status
NOT STARTED

## Depends On
- `009`

## Goal
Create low-overhead CPU/RSS/peak/I/O/free-space sampling primitives.

## Why This Task Exists
Resource acquisition should be independently testable before service scheduling.

## Allowed Files
- `Makefile`
- `Makefile.cross`
- `src/diagnostics/LinuxProcessMetrics.hpp`
- `src/diagnostics/LinuxProcessMetrics.cpp`
- `tests/cases/test_telemetry.inc`
- `tests/fixtures/telemetry/proc_io_sample.txt`
- `tests/fixtures/telemetry/proc_statm_sample.txt`

## Forbidden Scope
- No SDL-thread sampling.
- No /proc/self/smaps.
- No on-device CPU percentage.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create Snapshot with raw cumulative CPU us, RSS KiB, peak RSS KiB, read/write bytes, free bytes, validity flags.
2. CPU uses TelemetryClock::processCpuUs.
3. RSS parses resident pages from /proc/self/statm times page size.
4. Peak RSS uses getrusage(RUSAGE_SELF).ru_maxrss.
5. I/O parses only read_bytes and write_bytes from /proc/self/io.
6. Free storage uses statvfs and f_bavail*f_frsize.
7. Provide parser helpers for fixtures.
8. Malformed/missing metrics set invalid flags.
9. If a later I/O counter decreases, invalidate/reset baseline instead of producing a spike.
10. Add LinuxProcessMetrics.cpp to host/test and ARM lists.

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
make onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- No smaps.
- Malformed metrics are nonfatal.
- I/O regressions cannot create fake spikes.

## Commit
```sh
git add Makefile Makefile.cross src/diagnostics/LinuxProcessMetrics.hpp src/diagnostics/LinuxProcessMetrics.cpp tests/cases/test_telemetry.inc tests/fixtures/telemetry/proc_io_sample.txt tests/fixtures/telemetry/proc_statm_sample.txt
git commit -m "feat: add linux process metrics"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
