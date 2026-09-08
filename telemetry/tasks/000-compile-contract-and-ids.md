# Task 000 — establish compile contract and schema-stable IDs

## Status
NOT STARTED

## Depends On
- None

## Goal
Add the compile-time telemetry switch, exact MFT v1 enum/type definitions, and initial tests without starting telemetry.

## Why This Task Exists
The schema and disabled build mode must be fixed before any implementation code depends on them.

## Allowed Files
- `Makefile`
- `Makefile.cross`
- `src/diagnostics/TelemetryIds.hpp`
- `src/diagnostics/TelemetryTypes.hpp`
- `tests/test_main.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No PerformanceTelemetry implementation.
- No telemetry thread or trace file.
- No application instrumentation.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create `TelemetryIds.hpp` with every numeric enum exactly as `telemetry/SCHEMA_V1.md`, including UiPhaseId, UiScopeId, PlaybackState, WorkerId, RequestKind, ArtworkContext, SessionEventKind, WriterErrorKind, and SamplingReason. Do not add ConnectivityMode.
2. Create `TelemetryTypes.hpp` with fixed-width trivially-copyable producer payloads for all record types and a fixed tagged TelemetryRecord. Add static assertions for trivial copyability and `sizeof(TelemetryRecord) <= 96`.
3. Add `PERF_TELEMETRY ?= 1` to Makefile and Makefile.cross and define `MIYOOFIN_ENABLE_PERF_TELEMETRY=$(PERF_TELEMETRY)` for C++ compilation.
4. In Makefile create enabled-only `TELEMETRY_SRCS` and `TELEMETRY_TEST_SRCS`; in Makefile.cross create enabled-only `TELEMETRY_SRCS`. Keep them empty in this task.
5. Modify the outer `onionos` recipe so it passes `PERF_TELEMETRY=$(PERF_TELEMETRY)` to the inner Makefile.cross invocation.
6. Create telemetry tests for exact enum values, UiScope IDs, worker-mask bit constants, record payload size, and trivially-copyable guarantees.
7. Include and invoke `testTelemetrySchemaTypes()` from tests/test_main.cpp.
8. Because outputs are shared across flags, clean before each compile-variant switch.

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
make PERF_TELEMETRY=0 onionos
make verify-arm
make clean
make PERF_TELEMETRY=1 onionos
make verify-arm
make clean
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- Both compile variants are fresh builds.
- PERF_TELEMETRY reaches Makefile.cross.
- Every schema enum matches SCHEMA_V1.md.
- No runtime telemetry exists yet.

## Commit
```sh
git add Makefile Makefile.cross src/diagnostics/TelemetryIds.hpp src/diagnostics/TelemetryTypes.hpp tests/test_main.cpp tests/cases/test_telemetry.inc
git commit -m "build: establish telemetry compile contract"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
