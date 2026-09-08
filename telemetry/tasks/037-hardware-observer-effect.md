# Task 037 — run real Miyoo A B C and low-storage validation

## Status
NOT STARTED

## Depends On
- `036`

## Goal
Execute physical-device observer-effect, long-session, and low-storage validation and record evidence.

## Why This Task Exists
Telemetry is unfinished until its impact is measured on the actual Miyoo Mini Plus.

## Allowed Files
- `docs/performance-telemetry-benchmark.md`

## Forbidden Scope
- No application-code tuning.
- No condition changes between A/B/C.
- No direct miyoofin launch.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
# STOP if the physical Miyoo Mini Plus and controlled benchmark setup are unavailable.
```

## Exact Steps
1. Record same device/SD/OnionOS/firmware/Wi-Fi/Jellyfin/media/clock conditions.
2. Build A from clean outputs with PERF_TELEMETRY=0, validate ARM, deploy/package, launch via ./launch.sh.
3. Run make clean before switching to enabled build; build PERF_TELEMETRY=1 once, validate ARM, deploy/package.
4. Run B via MIYOOFIN_TELEMETRY=0 ./launch.sh and prove no telemetry thread/file.
5. Run C using exact same binary via MIYOOFIN_TELEMETRY=1 ./launch.sh.
6. Run idle, warm startup, controlled cold MiyooFin cache startup, aggressive Home, Episode artwork stress, library sync, download, network playback, local playback.
7. Run controlled low-storage crossing and verify terminal event/health, writer close, no further trace growth, app/download continuity.
8. Run 1–2 hour active soak.
9. Use five repetitions for short scenarios when practical and record median/spread.
10. Evaluate targets from BENCHMARK_PROTOCOL.md; if materially failed, record failure and STOP without declaring completion.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make clean
make PERF_TELEMETRY=0 onionos
make verify-arm
make clean
make PERF_TELEMETRY=1 onionos
make verify-arm
```

## Required Final Validation
```sh
make test -j2
git diff --check
```

## Acceptance Criteria
- Real device evidence recorded.
- B has no thread/file.
- C uses normal launcher.
- Low-storage shutdown proven.
- Observer effect explicitly PASS/FAIL.

## Commit
```sh
git add docs/performance-telemetry-benchmark.md
git commit -m "perf: validate telemetry on miyoo"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
