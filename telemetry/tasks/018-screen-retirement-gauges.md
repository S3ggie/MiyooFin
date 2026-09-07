# Task 018 — instrument ScreenStack retirement worker

## Status
NOT STARTED

## Depends On
- `017`

## Goal
Publish retirement active/depth/highwater/completion gauges.

## Why This Task Exists
Deferred destruction can correlate with CPU/latency while staying off SDL.

## Allowed Files
- `src/app/ScreenStack.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not change ScreenStack public API.
- No extra worker mutex.
- No lifetime change.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Inside existing RetirementQueue mutex boundaries update ScreenRetirement queue depth after push/pop.
2. Mark active around screen.reset and inactive afterward.
3. Increment completed after destruction.
4. Publish inactive/depth zero on shutdown.
5. Do not let service thread inspect pending deque.
6. Extend retirement test to preserve nonblocking pop behavior and gauge transitions.

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
- Retirement remains off SDL.
- No additional lock ownership.
- Checkpoint C ready.

## Commit
```sh
git add src/app/ScreenStack.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument screen retirement"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
