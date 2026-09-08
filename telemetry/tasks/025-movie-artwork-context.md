# Task 025 — establish MovieDetails artwork context

## Status
NOT STARTED

## Depends On
- `024`

## Goal
Give MovieDetails shared cache/decode/network operations safe semantic context.

## Why This Task Exists
MovieDetails uses the same shared artwork stack and should not appear as Unknown.

## Allowed Files
- `src/ui/screens/MovieDetailsScreen.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No worker redesign.
- No generic metric increments.
- No movie identity serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Wrap MovieDetails artwork preparation/load with TelemetryArtworkScope(MovieDetails).
2. Wrap direct artwork network operation with TelemetryRequestScope(RequestKind::Artwork).
3. Keep existing UiDiagnostics scopes/workers.
4. Do not add a new WorkerId.
5. Do not emit cache/decode/network records directly.

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
- MovieDetails gets safe enum context.
- No schema expansion.
- No duplicate measurements.

## Commit
```sh
git add src/ui/screens/MovieDetailsScreen.cpp tests/cases/test_telemetry.inc
git commit -m "feat: tag movie artwork telemetry context"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
