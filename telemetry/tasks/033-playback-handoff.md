# Task 033 — instrument playback handoff return and sampler suspension

## Status
NOT STARTED

## Depends On
- `032`

## Goal
Emit five playback timing stages and suspend periodic sampling during external child playback.

## Why This Task Exists
External playback intentionally blocks the parent and must not look like a giant UI stall/frame.

## Allowed Files
- `src/app/App.hpp`
- `src/app/App.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not modify launch/playback_runner scripts.
- Do not change fork/exec/wait order.
- Do not remove m_lastTick reset.
- Do not claim FFplay child CPU.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add minimal App bookkeeping for ephemeral playback sequence, request timestamp, return-pending flag; source remains Unknown if not safely known.
2. When external playback flag is consumed allocate sequence/capture request time before overlay.
3. Immediately after terminal Loading frame SDL_RenderPresent emit RequestToFinalPresent.
4. Time suspendPlatform with guarded timer and emit SuspendPlatform.
5. Call suspendSampling(true, ExternalPlayback) immediately before child wait and emit SamplingSuspended.
6. Measure waitpid interval and numeric exit kind/code as ChildWait.
7. Time resumePlatform and emit ResumePlatform.
8. Call suspendSampling(false, ExternalPlayback) after resume; reset periodic deadlines, no backfill.
9. At first later normal present emit ReturnToFirstNormalFrame and set PlaybackState UiActive.
10. Keep UiDiagnostics suspend/resume and m_lastTick reset unchanged.
11. Add deterministic milestone bookkeeping tests.

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
make onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- Five stages emitted.
- Sampler pause brackets child wait.
- No giant UI frame/stall.
- Checkpoint F ready.

## Commit
```sh
git add src/app/App.hpp src/app/App.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument playback telemetry"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
