# Task 031 — feed DownloadManager worker and DownloadSample gauges

## Status
NOT STARTED

## Depends On
- `030`

## Goal
Update facade-owned transfer/planner/reconcile gauges and byte deltas from existing owner code.

## Why This Task Exists
Task 012 already owns service emission; this task only feeds producer-side values.

## Allowed Files
- `src/download/DownloadManager.cpp`
- `src/download/DownloadManagerPlanning.cpp`
- `src/download/DownloadManagerReconcileWorker.cpp`
- `src/download/DownloadManagerTransfer.cpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- Do not modify PerformanceTelemetry.cpp.
- No service-thread DownloadManager lock.
- No ID/scope/title serialization.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Mark DownloadTransfer active when queued work is removed and inactive when transfer returns/waits.
2. Under existing manager mutex publish queued and active download counts.
3. On planner enqueue/pop publish planner queue depth; mark DownloadPlanner active during plan execution.
4. Represent reconcile pending as depth 0/1 and active during pass.
5. In recordProgress feed only positive accepted downloaded-byte delta to facade; never copy item ID/scope into telemetry.
6. Do not emit DownloadSample here; Task 012 already does it.
7. Do not add telemetry-specific manager lock/snapshot.

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
- Task does not touch service emission code.
- No service thread takes DownloadManager mutex.
- Three workers remain distinct.

## Commit
```sh
git add src/download/DownloadManager.cpp src/download/DownloadManagerPlanning.cpp src/download/DownloadManagerReconcileWorker.cpp src/download/DownloadManagerTransfer.cpp tests/cases/test_telemetry.inc
git commit -m "feat: feed download telemetry gauges"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
