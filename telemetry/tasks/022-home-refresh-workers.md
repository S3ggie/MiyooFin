# Task 022 — instrument Home lightweight refresh workers

## Status
NOT STARTED

## Depends On
- `021`

## Goal
Measure resume-refresh and download-snapshot refresh activity/duration.

## Why This Task Exists
Short refresh workers can overlap UI navigation and need distinct WorkerIds.

## Allowed Files
- `src/ui/screens/HomeScreenRefresh.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No cadence/state changes.
- No journal/cache path logging.
- No UI publication timing duplication.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Mark HomeResumeRefresh active/depth=1 at lambda entry and use guarded TelemetryTimer; finalize completed/failed and idle on every exit.
2. Do the same for HomeDownloadRefresh.
3. Exclude finishResumeRefresh/finishDownloadRefresh UI publication from worker duration.
4. Do not serialize paths, item IDs, server/user data.

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
- Both workers return idle on all exits.
- Runtime-off timer guarded.
- No sensitive path data.

## Commit
```sh
git add src/ui/screens/HomeScreenRefresh.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument home refresh workers"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
