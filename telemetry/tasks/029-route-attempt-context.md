# Task 029 — annotate RouteRequest LAN public and fallback attempts

## Status
NOT STARTED

## Depends On
- `028`

## Goal
Expose route/attempt/fallback context around each existing RouteRequest operation call.

## Why This Task Exists
A semantic request may execute two transport attempts.

## Allowed Files
- `src/net/RouteRequest.hpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No fallback classification change.
- No URL serialization.
- No transport record emission here.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Public-only path sets Public attempt=1 fallback=false.
2. First LAN path sets Lan attempt=1 fallback=false.
3. Existing public fallback sets Public attempt=2 fallback=true.
4. Context scope wraps only the operation call and restores immediately.
5. Keep RouteStatus and existing route prints unchanged.
6. Extend testRouteRequest for LAN success, public-only, nontransport no-fallback, and LAN transport failure/public fallback.

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
- Attempt context matches existing routing.
- No URL copied.
- Fallback rules unchanged.

## Commit
```sh
git add src/net/RouteRequest.hpp tests/cases/test_telemetry.inc
git commit -m "feat: annotate route attempts"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
