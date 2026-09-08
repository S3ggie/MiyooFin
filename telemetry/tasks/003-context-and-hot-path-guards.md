# Task 003 — create early enum-only context and hot-path timing guards

## Status
NOT STARTED

## Depends On
- `002`

## Goal
Create TelemetryContext and TelemetryTimer before any artwork/network task uses them.

## Why This Task Exists
The original roadmap created context too late; runtime-off also needs a guaranteed no-clock fast path.

## Allowed Files
- `src/diagnostics/TelemetryContext.hpp`
- `src/diagnostics/TelemetryGuards.hpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No Jellyfin annotation yet.
- No application instrumentation.
- No dynamic strings in context.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create thread-local enum-only context for RequestKind, RouteKind, attempt number, fallback flag, and ArtworkContext.
2. Create RAII Request/Route/Artwork scopes that save/restore prior enum values.
3. In enabled builds each scope checks enabledFast before touching TLS; runtime-off scopes leave TLS untouched.
4. Compile-out scope classes are inline no-ops and instantiate no telemetry TLS on hot paths.
5. Create TelemetryTimer whose enabled-build constructor first checks enabledFast and calls monotonicUs only when true; compile-out form is an inline inactive no-op.
6. Add tests for nested restore, runtime-off no-TLS mutation, runtime-off inactive timer, and compile-out build.

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
git diff --check
```

## Acceptance Criteria
- TelemetryContext exists before all artwork tasks.
- Runtime-off performs no telemetry clock/context work.
- Compile-out helpers are no-ops.

## Commit
```sh
git add src/diagnostics/TelemetryContext.hpp src/diagnostics/TelemetryGuards.hpp tests/cases/test_telemetry.inc
git commit -m "feat: add telemetry context guards"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
