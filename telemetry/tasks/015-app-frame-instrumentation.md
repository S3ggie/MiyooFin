# Task 015 — instrument App frame boundaries with runtime-off clock guard

## Status
NOT STARTED

## Depends On
- `014`

## Goal
Measure full frame, input, update, transition, render, framebuffer upload, and present/VSync at existing loop boundaries.

## Why This Task Exists
The main loop has exact boundaries, but runtime-off must not pay clock_gettime cost.

## Allowed Files
- `src/app/App.cpp`

## Forbidden Scope
- Do not reorder loop work.
- Do not change SDL dt semantics.
- Do not include external child wait in FullFrame.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Wrap all frame telemetry in compile-time telemetry guards so PERF_TELEMETRY=0 removes it.
2. At frame start take one `telemetryEnabled=enabledFast()` snapshot before any telemetry clock call.
3. Only when true call monotonicUs for phase boundaries.
4. Measure Input over current poll+action dispatch.
5. Measure Update over current update plus saved-session-validation completion without moving either.
6. Measure Transition over current screen-transition section.
7. Measure ScreenRender over Screen::render only.
8. Measure FramebufferUpload over SDL_UpdateTexture only.
9. Measure Present over renderer clear/copy/SDL_RenderPresent.
10. Record FullFrame at normal boundary, excluding external child wait.
11. Inspect final diff to ensure no TelemetryClock call can execute before the enabled flag check.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
make -j2
grep -n "TelemetryClock\|enabledFast" src/app/App.cpp
```

## Required Final Validation
```sh
make onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- Runtime-off does no telemetry clock calls in loop.
- Compile-out removes timing code.
- Phase order unchanged.

## Commit
```sh
git add src/app/App.cpp
git commit -m "feat: instrument app frame timing"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
