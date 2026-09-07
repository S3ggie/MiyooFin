# Task 026 — make ImageCache the sole generic cache metric owner

## Status
NOT STARTED

## Depends On
- `025`

## Goal
Record generic cache probe/read/write aggregate metrics and compressed bytes.

## Why This Task Exists
Central ownership prevents pipeline double counting and correlates SD activity.

## Allowed Files
- `src/cache/ImageCache.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No cache API/path change.
- No path/item/tag serialization.
- No additional cache lock.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Use compile-time guard plus runtime enabledFast before any optional telemetry timing.
2. In isCached feed exactly one hit/miss counter after stat result.
3. In readCached feed read success/failure and successful compressed bytes.
4. In writeToCache feed write success/failure and compressed bytes.
5. Do not emit per-operation MFT records; only feed ArtworkSummary interval counters.
6. Add aggregate test hooks/assertions using existing cache tests.
7. Inspect diff to ensure runtime-off does no unnecessary telemetry clock call.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
grep -n "TelemetryClock\|enabledFast\|Artwork" src/cache/ImageCache.cpp
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- ImageCache sole owner of generic cache counters.
- Runtime-off avoids unnecessary timing.
- No cache identity serialized.

## Commit
```sh
git add src/cache/ImageCache.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument image cache metrics"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
