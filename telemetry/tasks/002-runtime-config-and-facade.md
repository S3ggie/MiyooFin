# Task 002 — add runtime config and no-op facade

## Status
NOT STARTED

## Depends On
- `001`

## Goal
Create TelemetryConfig and PerformanceTelemetry public lifecycle with compile-out and runtime-off semantics, but no service thread yet.

## Why This Task Exists
Hot-path guards and context need a cheap authoritative enabled flag before any instrumentation.

## Allowed Files
- `Makefile`
- `Makefile.cross`
- `src/diagnostics/TelemetryConfig.hpp`
- `src/diagnostics/TelemetryConfig.cpp`
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No trace writer.
- No service thread.
- No application source changes.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create TelemetryConfig defaults: runtime off; sample 1000ms; free-space 10000ms; writer buffer 32768; flush 5000ms; rotate 16MiB; retain 4; min free 128MiB; target `/mnt/SDCARD/App/MiyooFin/telemetry-logs` with host-test override.
2. Implement `fromEnvironment()` so only `MIYOOFIN_TELEMETRY=1` enables telemetry. Accept `MIYOOFIN_TELEMETRY_MIN_FREE_MIB` only, clamped to 64–4096MiB.
3. Create enabled-build singleton API: start, stop, enabledFast, suspendSampling, nextEphemeralId. For this task start/stop only update in-memory state.
4. Make enabledFast a relaxed atomic bool load.
5. Under `MIYOOFIN_ENABLE_PERF_TELEMETRY=0`, provide inline no-op facade methods and compile-time false enabled behavior.
6. Add both new .cpp files to enabled telemetry source lists in Makefile and Makefile.cross and add diagnostics object directories in both builds.
7. Add tests for runtime default off, exact env enablement, min-free clamp, and nonzero ephemeral IDs.
8. Clean before changing compile variants.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make clean
make PERF_TELEMETRY=0 test -j2
make PERF_TELEMETRY=0 -j2
make clean
make PERF_TELEMETRY=1 test -j2
make PERF_TELEMETRY=1 -j2
```

## Required Final Validation
```sh
make clean
make PERF_TELEMETRY=1 onionos
make verify-arm
make clean
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- New .cpp files are wired to host/test and ARM builds.
- Runtime-off has no thread/file side effect.
- Compile-out does not link implementation.

## Commit
```sh
git add Makefile Makefile.cross src/diagnostics/TelemetryConfig.hpp src/diagnostics/TelemetryConfig.cpp src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp tests/cases/test_telemetry.inc
git commit -m "feat: add telemetry runtime facade"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
