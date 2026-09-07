# Task 007 — connect ring writer service thread rotation and core health

## Status
NOT STARTED

## Depends On
- `006`

## Goal
Create exactly one runtime-enabled service thread that drains records and owns trace rotation.

## Why This Task Exists
This completes the core transport before application domains begin producing records.

## Allowed Files
- `.gitignore`
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `src/diagnostics/TelemetryWriter.hpp`
- `src/diagnostics/TelemetryWriter.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No Linux metrics yet.
- No application instrumentation.
- No detached thread.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add one 512-slot ring, one joinable service thread, stop flag, session nonce, consumer sequence, drops/writer errors/rotation/sampling-late counters.
2. Add numeric/POD producer emit methods; every emitter checks enabledFast before timestamp/context/ring work.
3. Ring push failure increments cumulative drops and returns immediately.
4. start(config) spawns one service thread only when runtime enabled; runtime-off starts no thread/file.
5. Service thread opens writer, emits TelemetryStarted, drains/encodes/writes records.
6. Implement 16MiB rotation and four-file retention with same session nonce and incremented rotation index.
7. On clean stop, drain accepted records, emit TelemetryStopped, flush/close, and join.
8. Add `.mft` and runtime telemetry-log ignores without ignoring roadmap telemetry/.
9. Add tests for runtime-off, drain, forced drops, small-limit rotation, and clean join.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
make clean
make PERF_TELEMETRY=0 test -j2
make clean
make PERF_TELEMETRY=1 test -j2
```

## Required Final Validation
```sh
make -j2
make onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- Exactly one active service thread.
- Runtime-off creates no thread/file.
- Rotation bounded.
- Drops cannot block.

## Commit
```sh
git add .gitignore src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp src/diagnostics/TelemetryWriter.hpp src/diagnostics/TelemetryWriter.cpp tests/cases/test_telemetry.inc
git commit -m "feat: add telemetry service thread"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
