# Task 027 — make ImageDecoder the sole decode metric owner

## Status
NOT STARTED

## Depends On
- `026`

## Goal
Measure JPEG duration/input/output and feed individual ArtworkDecode plus interval ArtworkSummary.

## Why This Task Exists
One shared owner prevents duplicate decode records from pipelines.

## Allowed Files
- `src/image/ImageDecoder.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No stb behavior change.
- No path identity.
- No pipeline duplicate record.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Use TelemetryTimer so runtime-off performs only enabled check and no telemetry clock.
2. After decode compute success/failure, input size, overflow-safe width*height*4 output size.
3. Read current ArtworkContext from TelemetryContext.
4. Emit exactly one ArtworkDecode here.
5. Feed ArtworkSummary decode count/failure/total/max only here.
6. Do not perform extra decode/copy.
7. Extend valid/invalid JPEG tests.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
grep -n "TelemetryTimer\|ArtworkDecode\|enabledFast" src/image/ImageDecoder.cpp
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- Exactly one shared decode record owner.
- Runtime-off timer inactive.
- JPEG behavior unchanged.

## Commit
```sh
git add src/image/ImageDecoder.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument image decoder metrics"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
