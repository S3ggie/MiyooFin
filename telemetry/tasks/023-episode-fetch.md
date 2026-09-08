# Task 023 — instrument EpisodeBrowser fetch worker

## Status
NOT STARTED

## Depends On
- `022`

## Goal
Measure EpisodeFetch activity/outcome without identifying content.

## Why This Task Exists
Episode fetch combines cache/catalog/network work on one worker.

## Allowed Files
- `src/ui/screens/EpisodeBrowserScreen.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No cancellation/persistence changes.
- No series/season/episode IDs.
- No generic artwork metric increments.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Mark EpisodeFetch active/depth=1 and start guarded timer at thread entry.
2. Classify cached/offline success, network success, cancellation, failure numerically.
3. Finalize inactive/depth=0 on every early/normal exit.
4. Keep publishEpisodes UI work untouched.

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
- Cancellation differs from failure.
- No worker-active leak.
- Existing Episode tests pass.

## Commit
```sh
git add src/ui/screens/EpisodeBrowserScreen.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument episode fetch worker"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
