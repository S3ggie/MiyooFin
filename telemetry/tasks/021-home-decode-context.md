# Task 021 — instrument Home decode worker and establish decode context

## Status
NOT STARTED

## Depends On
- `020`

## Goal
Measure HomeDecode queue/activity while assigning enum-only context to shared cache/decode calls.

## Why This Task Exists
Home 32-job decode pressure must be visible without double counting shared operations.

## Allowed Files
- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenArtwork.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not change queue cap 32.
- Do not emit cache/decode metrics here.
- No artwork key serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add private ArtworkContext field to DecodeJob only for telemetry semantics.
2. Assign HomeShows, HomeSelected, or HomeGrid context at submitDecode based on existing job role.
3. Publish HomeDecode queue depth after successful enqueue under existing mutex.
4. When existing size>=32 guard rejects, increment worker failure/drop-style counter only and preserve exact guard/order.
5. After worker pop update depth/active.
6. Wrap ImageCache read and ImageDecoder decode with TelemetryArtworkScope(job.context).
7. Do not increment ArtworkSummary or emit ArtworkDecode here.
8. Count stale discarded Shows results as cancelled.

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
- 32-job cap unchanged.
- Header is explicitly allowed for context field.
- Shared layers receive context.
- No double counting.

## Commit
```sh
git add src/ui/screens/HomeScreen.hpp src/ui/screens/HomeScreenArtwork.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument home decode worker"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
