# Task 028 — annotate Jellyfin semantic RequestKind context

## Status
NOT STARTED

## Depends On
- `027`

## Goal
Assign safe RequestKind to every Jellyfin transport operation without changing public signatures.

## Why This Task Exists
HttpClient must never parse URLs to infer semantics.

## Allowed Files
- `src/net/JellyfinApiAuth.cpp`
- `src/net/JellyfinApiLibrary.cpp`
- `src/net/JellyfinApiHierarchy.cpp`
- `src/net/JellyfinApiPlayback.cpp`
- `src/net/JellyfinApiDownload.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No URL/body/header changes.
- No NetworkRequest emission here.
- No public API signature changes.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Annotate SystemInfo, Authentication, TokenValidation.
2. Annotate Views, LibraryItems, ResumeItems, LatestItems, ChangedHierarchy.
3. Annotate Seasons and Episodes.
4. Annotate PlaybackPosition and PlaybackStopped.
5. Annotate DownloadPlaybackInfo, HlsMaster, HlsVariant according to current call sites.
6. Do not emit transport records here.
7. Add nested context restoration tests.

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
git diff --check
```

## Acceptance Criteria
- Every Jellyfin transport has semantic context.
- No endpoint behavior change.
- No URL parsing.

## Commit
```sh
git add src/net/JellyfinApiAuth.cpp src/net/JellyfinApiLibrary.cpp src/net/JellyfinApiHierarchy.cpp src/net/JellyfinApiPlayback.cpp src/net/JellyfinApiDownload.cpp tests/cases/test_telemetry.inc
git commit -m "feat: annotate jellyfin request telemetry"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
