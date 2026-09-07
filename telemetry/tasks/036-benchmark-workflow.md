# Task 036 — add repository benchmark workflow and comparison tool

## Status
NOT STARTED

## Depends On
- `035`

## Goal
Create repository-facing telemetry docs, benchmark result template, and A/B/C comparison tooling.

## Why This Task Exists
Exact hardware commands/conditions must exist before data collection.

## Allowed Files
- `docs/performance-telemetry.md`
- `docs/performance-telemetry-benchmark.md`
- `tools/telemetry/README.md`
- `tools/telemetry/compare_runs.py`
- `tests/test_telemetry_decoder.py`

## Forbidden Scope
- Do not claim hardware results.
- Do not modify launch.sh.
- Do not change telemetry defaults.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Document compile/runtime enablement, trace path, copy/decode/analyze, low-storage cutoff, rotation, privacy.
2. Create result template with environment, scenario repetitions, medians/spread, A-vs-B/B-vs-C deltas, low-storage result, initial NOT RUN status.
3. Document mandatory make clean before changing PERF_TELEMETRY compile variant.
4. Document B/C use the same compiled-in binary.
5. Document normal Onion launch commands: A `./launch.sh`, B `MIYOOFIN_TELEMETRY=0 ./launch.sh`, C `MIYOOFIN_TELEMETRY=1 ./launch.sh`.
6. Create compare_runs.py observer-effect calculations and self-test.
7. Never instruct direct ./miyoofin benchmark launch.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
python3 tools/telemetry/compare_runs.py --self-test
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- Normal Onion launcher used.
- Clean-rebuild rule explicit.
- No hardware PASS claim.

## Commit
```sh
git add docs/performance-telemetry.md docs/performance-telemetry-benchmark.md tools/telemetry/README.md tools/telemetry/compare_runs.py tests/test_telemetry_decoder.py
git commit -m "docs: add telemetry benchmark workflow"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
