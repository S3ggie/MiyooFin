# Task 016 — record safe UI and playback state transitions

## Status
NOT STARTED

## Depends On
- `015`

## Goal
Emit enum-only screen/tab/action/PlaybackState transitions and suppress duplicates.

## Why This Task Exists
Resource/timing records need semantic state correlation.

## Allowed Files
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `src/app/App.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No diagnostic strings in trace.
- No ConnectivityMode.
- No navigation behavior change.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add fixed mapping helpers for screen diagnostic names, Home tabs, and Action; unknown maps to Other and source text is not stored.
2. Facade setters compare previous enum and emit StateTransition only on changes.
3. Update screen/tab/action at existing App observation points.
4. Use exact PlaybackState values: UiActive, StartingOverlay, ExternalPlayback, Resuming.
5. Add duplicate suppression and unknown mapping tests.

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
- No ConnectivityMode.
- Duplicate states suppressed.
- No source string copied.

## Commit
```sh
git add src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp src/app/App.cpp tests/cases/test_telemetry.inc
git commit -m "feat: record telemetry state transitions"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
