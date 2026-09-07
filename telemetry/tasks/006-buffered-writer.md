# Task 006 — add buffered trace writer

## Status
NOT STARTED

## Depends On
- `005`

## Goal
Create consumer-owned MFT file buffering and clean close behavior.

## Why This Task Exists
All SD-card writes must remain behind the service-thread boundary.

## Allowed Files
- `Makefile`
- `Makefile.cross`
- `src/diagnostics/TelemetryWriter.hpp`
- `src/diagnostics/TelemetryWriter.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No service thread yet.
- No rotation yet.
- No producer writer calls.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Create writer open/append/flushIfDue/flush/close/isOpen/logicalBytesWritten/bufferedBytes APIs.
2. Allocate configured writer buffer once in open.
3. Create directory/file only inside writer open; write MFT header first.
4. Flush on 32KiB/full buffer or configured 5s deadline; never fsync per record/interval.
5. Clean close flushes and closes.
6. Expose numeric WriterErrorKind and errno only; no error string enters trace.
7. Add temporary-directory tests for valid header, buffered persistence, close flush, and error classification.
8. Add TelemetryWriter.cpp to host/test and ARM telemetry source lists.

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
- Only consumer-side writer ownership.
- No per-record durable sync.
- Numeric-only writer errors.

## Commit
```sh
git add Makefile Makefile.cross src/diagnostics/TelemetryWriter.hpp src/diagnostics/TelemetryWriter.cpp tests/cases/test_telemetry.inc
git commit -m "feat: add buffered telemetry writer"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
