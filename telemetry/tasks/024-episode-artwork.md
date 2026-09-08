# Task 024 — instrument Episode artwork worker and contexts

## Status
NOT STARTED

## Depends On
- `023`

## Goal
Measure EpisodeArtwork scheduling/cancellation while setting safe request/decode context.

## Why This Task Exists
The worker deliberately preempts stale work; actual generic cache/decode metrics belong to shared layers.

## Allowed Files
- `src/ui/screens/EpisodeBrowserArtwork.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No generation/cancel changes.
- No generic cache/decode metric increments.
- No job key/ID/tag/URL serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Mark EpisodeArtwork active only while processing candidate and idle while waiting/window warm.
2. Publish effective queue demand from eligible visible candidates only while existing worker state is locked.
3. Classify stale generation/callback abort as cancelled.
4. Wrap direct artwork network call in TelemetryRequestScope(RequestKind::Artwork).
5. Use TelemetryArtworkScope(EpisodeSelected) for selected cache/decode and EpisodePrefetch for prefetch network semantics.
6. Do not emit ArtworkDecode or update ArtworkSummary here.
7. Preserve existing stale-job decode avoidance.

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
- Preemption unchanged.
- Artwork request context established.
- No duplicate cache/decode metrics.

## Commit
```sh
git add src/ui/screens/EpisodeBrowserArtwork.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument episode artwork worker"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
