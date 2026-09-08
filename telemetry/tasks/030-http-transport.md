# Task 030 — make HttpClient the sole normal transport measurement owner

## Status
NOT STARTED

## Depends On
- `029`

## Goal
Emit one NetworkRequest per normal/binary libcurl attempt with duration/status/CURLcode/payload counts.

## Why This Task Exists
Transport metrics belong at actual curl execution, not semantic layers.

## Allowed Files
- `src/net/HttpClient.hpp`
- `src/net/HttpClient.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No URL/header/body/error serialization.
- No TLS/timeout/cancel behavior changes.
- No HLS direct segment metrics here.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Add numeric transportCode to BinaryHttpResponse with source-compatible default.
2. Use TelemetryTimer around each curl_easy_perform; runtime-off timer does not clock.
3. After perform emit one NetworkRequest using current RequestKind/RouteKind/attempt/fallback.
4. Text rx bytes=response.body.size and tx body=postBody.size.
5. Binary rx bytes=retained data size; truncation/cancel flags use existing state/CURLcode.
6. If semantic/route context absent use Unknown; never inspect URL.
7. Emit for success and transport-failure paths that actually called curl_easy_perform.
8. Use local deterministic tests only; no public internet.
9. Inspect diff to prove URL, headers, bodies, and error strings are not passed to telemetry.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
grep -n "TelemetryTimer\|NetworkRequest\|url\|headers\|error" src/net/HttpClient.cpp
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
- One record per actual HttpClient attempt.
- Runtime-off avoids clock.
- No secrets cross boundary.
- Checkpoint D ready.

## Commit
```sh
git add src/net/HttpClient.hpp src/net/HttpClient.cpp tests/cases/test_telemetry.inc
git commit -m "feat: instrument http transport"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
