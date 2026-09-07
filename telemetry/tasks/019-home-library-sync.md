# Task 019 — instrument Home library sync worker

## Status
NOT STARTED

## Depends On
- `018`

## Goal
Record HomeLibraryFetch worker state and one LibrarySync summary per generation.

## Why This Task Exists
Full Home sync is a top-level semantic operation separate from individual requests.

## Allowed Files
- `src/ui/screens/HomeScreenSync.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No fetch/cache order changes.
- No library IDs/names.
- No cache/decode aggregate increments.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Use TelemetryTimer at fetch worker entry; runtime-off timer remains inactive.
2. Mark HomeLibraryFetch active/depth=1 at entry and inactive/depth=0 on every exit.
3. Track only view count, top-level media count, changed-hierarchy count, semantic request count, cache_saved, outcome.
4. Emit exactly one LibrarySync summary on success/fail/cancel completion.
5. Exclude UI-side finishFetch publication duration.
6. Do not feed ImageCache/ImageDecoder aggregate counters here.

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
- One summary per generation.
- No identifying library data.
- Worker cannot remain active.

## Commit
```sh
git add src/ui/screens/HomeScreenSync.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument home library sync"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
