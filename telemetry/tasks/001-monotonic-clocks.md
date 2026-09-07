# Task 001 — add monotonic telemetry clocks

## Status
NOT STARTED

## Depends On
- `000`

## Goal
Provide one monotonic correlation clock and one cumulative process-CPU clock.

## Why This Task Exists
All later spans/samples require exact monotonic timing primitives before instrumentation.

## Allowed Files
- `src/diagnostics/TelemetryClock.hpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not replace SDL timing.
- Do not change UiDiagnostics clock.
- No thread.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create header-only TelemetryClock.
2. Implement `monotonicUs()` with `clock_gettime(CLOCK_MONOTONIC)` and integer microsecond conversion.
3. Implement `processCpuUs()` with `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`.
4. Return zero only on syscall failure; never fall back to wall time.
5. Add non-regression host tests for both clocks.

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
- Correlation time is monotonic only.
- Existing SDL and UiDiagnostics timing remains untouched.

## Commit
```sh
git add src/diagnostics/TelemetryClock.hpp tests/cases/test_telemetry.inc
git commit -m "feat: add telemetry monotonic clocks"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
