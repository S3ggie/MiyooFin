# Task 032 — instrument direct HLS segment attempts retries and throughput

## Status
NOT STARTED

## Depends On
- `031`

## Goal
Emit safe direct HLS attempt records and feed segment/retry counters without changing proven transfer behavior.

## Why This Task Exists
HLS bypasses HttpClient, so direct instrumentation is required.

## Allowed Files
- `src/download/DownloadManagerTransfer.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not change five attempts.
- Do not change retryability/backoff/fallback/TLS/file semantics.
- No URL/path/ID/scope serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Allocate ephemeral telemetry_job_seq at transfer start; never derive from item ID.
2. Use TelemetryTimer immediately around each direct segment curl_easy_perform; runtime-off timer does not clock.
3. Record primary route from existing route decision without storing URL.
4. If LAN transport failure runs existing public fallback, emit LAN attempt then independently time/emit public fallback with same job/segment/attempt.
5. Use existing completed part/progress byte result; do not add file reads just for telemetry.
6. After existing hlsSegmentShouldRetry result, record retry_planned and exact `(1u << (attempt-1))*1000`ms delay; do not decide retry independently.
7. Increment segment-retried only when existing code will actually wait/retry and segment-completed only on existing success completion path.
8. Keep five-attempt loop, cancellation, part replacement, rename, reconciliation, persistence order unchanged.
9. Add pure payload builder tests and inspect runtime-off guard.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
grep -n "TelemetryTimer\|HLS_SEGMENT_ATTEMPTS\|hlsSegmentShouldRetry" src/download/DownloadManagerTransfer.cpp
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
- Five attempts unchanged.
- Fallback/retry order unchanged.
- No sensitive HLS identity.
- Checkpoint E ready.

## Commit
```sh
git add src/download/DownloadManagerTransfer.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument hls telemetry"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
