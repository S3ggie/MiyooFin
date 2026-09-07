# Task 004 — implement the bounded ARM-safe MPSC ring

## Status
NOT STARTED

## Depends On
- `003`

## Goal
Implement the exact fixed 512-slot nonblocking queue for telemetry producers.

## Why This Task Exists
This is high-risk concurrency code and must use a specified algorithm rather than model-invented lock-free logic.

## Allowed Files
- `src/diagnostics/TelemetryRing.hpp`
- `tests/cases/test_telemetry.inc`

## Forbidden Scope
- No producer mutex or condition variable.
- No unbounded spin/retry.
- No per-record allocation.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Implement power-of-two TelemetryRing<Capacity>; require Capacity<2^31; production capacity is 512.
2. Each slot is `{ atomic<uint32_t> sequence; TelemetryRecord record; }`; enqueue/dequeue positions are uint32_t.
3. Initialize slot[i].sequence=i, enqueuePos=0, dequeuePos=0.
4. Producer: relaxed-load enqueuePos; acquire-load slot sequence; compute signed `int32_t diff = int32_t(seq-pos)`. diff<0 returns full=false immediately; diff==0 attempts reservation; diff>0 reloads position.
5. Reserve with compare_exchange_weak(pos,pos+1, relaxed, relaxed). Permit at most 8 total reservation/CAS iterations; after 8 contention retries return false.
6. After reservation copy the fixed record, update approximate depth/highwater with bounded relaxed atomics, then publish with sequence.store(pos+1, release).
7. Single consumer: acquire-load sequence for local dequeue pos and compute `int32_t diff=int32_t(seq-(pos+1))`; diff==0 consumes, otherwise this poll returns false. After copy, release slot with sequence.store(pos+Capacity, release), decrement approximate depth, increment local dequeue pos.
8. Document memory ordering: producer release publishes record; consumer acquire sees it; consumer release exposes reuse; producer acquire sees reuse; position CAS only arbitrates tickets.
9. Document modulo wrap assumption: producer-consumer distance stays far below 2^31 because capacity is 512 plus bounded contenders.
10. Add FIFO/full/recovery/multi-producer stress/bounded-contention tests and a test-only near-UINT32_MAX seed crossing wrap.

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
- Only uint32_t queue atomics.
- Producer has a hard bounded retry count.
- Acquire/release ordering matches task text.
- Wraparound test crosses UINT32_MAX.

## Commit
```sh
git add src/diagnostics/TelemetryRing.hpp tests/cases/test_telemetry.inc
git commit -m "feat: add bounded telemetry ring"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
