# Task 017 — bridge UiDiagnostics with exact phase scope and worker-mask IDs

## Status
NOT STARTED

## Depends On
- `016`

## Goal
Forward existing slow/stall observations numerically while leaving UiDiagnostics authoritative.

## Why This Task Exists
Duplicating stall detection would create divergent semantics.

## Allowed Files
- `src/app/UiDiagnostics.hpp`
- `src/app/UiDiagnostics.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not change 500ms/100ms thresholds.
- Do not remove ui-stall.log.
- Do not serialize formatted diagnostic text.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Implement allowlist mapping from current phase strings to exact UiPhaseId values.
2. Implement allowlist mapping from current scope strings to exact UiScopeId values; unknown=0.
3. On existing slow threshold emit SlowScope with numeric scope/duration after preserving existing behavior.
4. On stall begin snapshot mapped phase/screen/tab/action/scope plus `activeWorkerMask()` exact bits.
5. On stall end emit End with existing duration.
6. Do not pass formatted stall/recent-event strings to telemetry.
7. Extend tests for exact mapping and worker mask.

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
- Thresholds/logging unchanged.
- Schema UiPhase/UiScope mapping exact.
- worker_mask uses exact bit table.

## Commit
```sh
git add src/app/UiDiagnostics.hpp src/app/UiDiagnostics.cpp tests/cases/test_telemetry.inc
git commit -m "feat: bridge ui diagnostics telemetry"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
