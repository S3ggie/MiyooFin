# Task 008 — integrate telemetry into the real process lifecycle

## Status
NOT STARTED

## Depends On
- `007`

## Goal
Wire TelemetryConfig::fromEnvironment and PerformanceTelemetry start/stop into src/main.cpp with correct App destruction and init-failure cleanup.

## Why This Task Exists
Without this task the subsystem is never active in the real process, and current early init failure would bypass cleanup.

## Allowed Files
- `src/main.cpp`

## Forbidden Scope
- Do not move telemetry lifecycle into App.
- Do not change SDL diagnostic prints.
- Telemetry startup failure must never make app startup fatal.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Include compile-out-safe telemetry config/facade headers.
2. After existing stdout/stderr setup and current SDL driver diagnostics, call TelemetryConfig::fromEnvironment then performanceTelemetry().start(config).
3. Replace current immediate App-init failure return with a result variable.
4. Put App construction/init/run inside an inner scope so App destructor runs before telemetry stop.
5. On init failure keep the existing error print, set result=1, skip app.run, leave App scope, then call performanceTelemetry().stop().
6. On success run app, leave App scope, then stop telemetry.
7. Keep final exit-code print/return semantics.
8. Confirm compile-out start/stop are no-ops and runtime-off makes no thread/file.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make clean
make PERF_TELEMETRY=0 -j2
make clean
make PERF_TELEMETRY=1 -j2
```

## Required Final Validation
```sh
make clean
make PERF_TELEMETRY=1 test -j2
make PERF_TELEMETRY=1 -j2
make PERF_TELEMETRY=1 onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- Real process calls start and stop.
- App destruction precedes telemetry stop on success and init failure.
- Telemetry failure is nonfatal.

## Commit
```sh
git add src/main.cpp
git commit -m "feat: wire telemetry process lifecycle"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
