# Task 005 — implement exact MFT v1 codec

## Status
NOT STARTED

## Depends On
- `004`

## Goal
Encode the normative binary header and all record types explicitly little-endian.

## Why This Task Exists
Writer and decoder need one stable byte contract before persistence begins.

## Allowed Files
- `Makefile`
- `Makefile.cross`
- `src/diagnostics/MftFormat.hpp`
- `src/diagnostics/MftFormat.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No file I/O.
- No raw C++ struct writes.
- No schema extensions.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create explicit little-endian helpers for u8/u16/u32/i32/u64.
2. Define file header=80, common record header=16, max record size=256.
3. Encode every file-header field exactly from SCHEMA_V1.md.
4. Encode all 14 record types using corrected normative sizes, including FrameTimingSummary=88 total and ArtworkSummary=80 total.
5. Reject any encoded record larger than 256 bytes.
6. Add golden byte tests for representative records including UiStall and SessionEvent.
7. Add MftFormat.cpp to both host/test and ARM telemetry source lists.

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
make onionos
make verify-arm
git diff --check
```

## Acceptance Criteria
- Host and ARM link codec.
- Golden bytes match schema.
- No ABI padding serialization.

## Commit
```sh
git add Makefile Makefile.cross src/diagnostics/MftFormat.hpp src/diagnostics/MftFormat.cpp tests/cases/test_telemetry.inc
git commit -m "feat: define mft v1 codec"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
