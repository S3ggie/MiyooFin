# Task 020 — instrument Home hierarchy and poster workers with artwork request context

## Status
NOT STARTED

## Depends On
- `019`

## Goal
Publish Home hierarchy/poster gauges and establish Artwork RequestKind for direct poster HTTP.

## Why This Task Exists
Poster and hierarchy are separate workers; poster transport otherwise lacks safe semantic context.

## Allowed Files
- `src/ui/screens/HomeScreenHierarchy.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No queue behavior changes.
- No generic cache/decode metric increments.
- No item/tag/URL serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Publish HomePoster queue depth/highwater while m_posterMutex is already held after deduplication.
2. Mark HomePoster active around a batch and update completed/failed counters.
3. Publish HomeHierarchy generation depth under m_hierarchyMutex and active/completed/failed/cancelled through existing generation lifecycle.
4. Wrap direct poster HTTP operation in TelemetryRequestScope(RequestKind::Artwork) and TelemetryArtworkScope(HomePoster).
5. Do not count generic ImageCache or ImageDecoder metrics here.

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
- Poster/hierarchy distinct WorkerIds.
- Poster transport will be semantic Artwork.
- No duplicate generic artwork metrics.

## Commit
```sh
git add src/ui/screens/HomeScreenHierarchy.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument home hierarchy poster workers"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
