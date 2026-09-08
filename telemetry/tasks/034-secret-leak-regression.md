# Task 034 — add raw MFT secret-leak regression

## Status
NOT STARTED

## Depends On
- `033`

## Goal
Prove fake secrets/identifiers never appear in generated trace bytes.

## Why This Task Exists
Privacy needs a binary regression rather than code review alone.

## Allowed Files
- `src/diagnostics/PerformanceTelemetry.hpp`
- `src/diagnostics/PerformanceTelemetry.cpp`
- `tests/test_main.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No redaction/string writer API.
- No public network.
- No schema weakening.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create host test with every fake secret from SECURITY.md held in realistic local variables.
2. Exercise safe state/worker/artwork/network/download/playback/health/session APIs while secrets exist.
3. Generate and close a real MFT through facade/writer.
4. Read raw bytes and assert absence of every fake full string plus SECRET_, private.example, api_key=.
5. If deterministic drain/flush test hook is needed, expose only numeric/control method accepting no arbitrary string.
6. Keep test in normal make test.

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
- Every fake string absent.
- Test uses real writer path.
- No generic string API.

## Commit
```sh
git add src/diagnostics/PerformanceTelemetry.hpp src/diagnostics/PerformanceTelemetry.cpp tests/test_main.cpp tests/cases/test_telemetry.inc
git commit -m "test: prevent telemetry secret leaks"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
