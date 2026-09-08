# Task 009 — add desktop decoder and golden traces

## Status
NOT STARTED

## Depends On
- `008`

## Goal
Create independent standard-library Python MFT decoding and golden files.

## Why This Task Exists
Independent decoding catches format drift before instrumentation expands.

## Allowed Files
- `Makefile`
- `tools/telemetry/schema.py`
- `tools/telemetry/decode.py`
- `tools/telemetry/README.md`
- `tests/test_telemetry_decoder.py`
- `tests/fixtures/telemetry/valid_v1.mft`
- `tests/fixtures/telemetry/truncated_v1.mft`

## Forbidden Scope
- No plotting yet.
- No third-party Python dependency.
- No schema changes.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create schema.py with exact corrected record layouts/enums.
2. Create decode.py header validation, known-record decoding, valid-size unknown skip, impossible-size stop, partial-tail ignore.
3. Support JSON and CSV-directory exports.
4. Create valid_v1.mft covering all 14 record types and exact UiPhase/UiScope/PlaybackState/SessionEvent semantics.
5. Create truncated_v1.mft by truncating a final record.
6. Add Python tests for exact values, unknown skip, and partial tail.
7. Run decoder tests from normal make test.

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
- Decoder matches corrected schema.
- Partial tail preserves prior complete records.
- Normal make test includes decoder test.

## Commit
```sh
git add Makefile tools/telemetry/schema.py tools/telemetry/decode.py tools/telemetry/README.md tests/test_telemetry_decoder.py tests/fixtures/telemetry/valid_v1.mft tests/fixtures/telemetry/truncated_v1.mft
git commit -m "tools: add mft v1 decoder"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
