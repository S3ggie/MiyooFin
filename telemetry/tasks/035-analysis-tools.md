# Task 035 — add laptop analysis correlation and exports

## Status
NOT STARTED

## Depends On
- `034`

## Goal
Create desktop summaries, CSV exports, and state/resource correlation reports.

## Why This Task Exists
Expensive interpretation belongs on the laptop, not the Miyoo.

## Allowed Files
- `tools/telemetry/schema.py`
- `tools/telemetry/decode.py`
- `tools/telemetry/analyze.py`
- `tools/telemetry/README.md`
- `tests/test_telemetry_decoder.py`

## Forbidden Scope
- No device schema changes.
- No mandatory third-party dependency.
- No device analysis.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Build analyze.py on decoded monotonic timeline.
2. Derive one-core CPU percent from process CPU/monotonic deltas; optionally whole-device share when CPU count supplied.
3. Summarize RSS/peak, I/O rates, free-space, frame histogram/max/stalls, workers, request latency, artwork, download, playback, health.
4. Export normalized CSV tables.
5. Generate correlations associating slow/stall timestamps with latest state and overlapping worker/network/download events.
6. Optional plots may use matplotlib only when installed; core remains standard library.
7. Add deterministic math/grouping tests.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
python3 tests/test_telemetry_decoder.py
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- All telemetry domains represented.
- Core tool standard-library only.
- Derived rates happen on laptop.

## Commit
```sh
git add tools/telemetry/schema.py tools/telemetry/decode.py tools/telemetry/analyze.py tools/telemetry/README.md tests/test_telemetry_decoder.py
git commit -m "tools: add telemetry analysis"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
