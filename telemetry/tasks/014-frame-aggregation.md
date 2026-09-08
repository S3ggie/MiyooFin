# Task 014 — add in-memory frame timing aggregation

## Status
NOT STARTED

## Depends On
- `013`

## Goal
Aggregate the seven frame phases in RAM and emit only interval summaries.

## Why This Task Exists
Per-frame trace records would create excessive queue and SD overhead.

## Allowed Files
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not touch App yet.
- No per-frame persistent record.
- No dynamic histogram.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add fixed accumulators for FullFrame, Input, Update, Transition, ScreenRender, FramebufferUpload, Present.
2. `recordFramePhase` updates count,total,max,>50ms,>100ms and exact nine histogram bins with bounded nonblocking atomics.
3. At service tick snapshot/reset each nonempty phase and emit FrameTimingSummary directly.
4. Use exact schema histogram boundaries.
5. Add boundary and interval reset tests.

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
- No per-frame ring record.
- Bins match schema.
- No SDL-path mutex.

## Commit
```sh
git add src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp tests/cases/test_telemetry.inc
git commit -m "feat: add frame timing aggregation"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
