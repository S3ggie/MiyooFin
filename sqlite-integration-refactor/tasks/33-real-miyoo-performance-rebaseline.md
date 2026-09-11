# Task 33 — Rebaseline SQLite integration performance on real Miyoo

**Phase:** D — performance rebaseline

## Objective

Measure the final refactored architecture on physical Miyoo hardware and record cold/warm startup, network, SQLite, artwork, memory, CPU, and telemetry health without optimization changes.

## Why

Performance must be measured after correctness/ownership cleanup before any speed or durability tradeoff is justified.

## Preconditions

- Task 32 passes.
- Physical Miyoo Mini Plus is available.
- Use the exact post-Task-32 commit for compared runs.

## Allowed Files

- Create or update `docs/sqlite-integration-refactor-benchmark.md` with factual measurements only
- No production source files

## Forbidden Scope

- Do not patch production code in this benchmark task; discovered issues become separate reviewed tasks.
- Do not change `journal_mode`, `synchronous`, busy timeout, schema, page size, worker count, or route policy.
- Do not include playback repair.
- Do not start original SQLite Task 34 automatically.

## Architecture invariants

- Schema v3 remains the baseline. Do not introduce schema v4 or redesign the schema unless this task explicitly requires it; no task in this roadmap currently does.
- Exactly one `CatalogDb` SQLite worker owns exactly one SQLite connection for the active scope.
- No SQLite operation, HTTP request, long filesystem operation, retry sleep, or blocking worker join may run on the SDL/UI thread.
- Keep indexed/keyset bounded reads and bounded result windows; do not reintroduce whole-library RAM materialization.
- Keep the atomic fresh-database temporary-file bootstrap/promotion path unchanged unless a task explicitly targets it.
- `DownloadStore` remains authoritative for physical offline availability. Catalog metadata may enrich downloads but must not decide whether bytes exist.
- `ImageCache` remains the artwork-byte cache and is not replaced by SQLite.
- Do not add production dual-write to legacy whole-file catalog/cache persistence.
- The intermittent audio-only FFplay/mmiyoo display bug is out of scope. Do not modify playback repair code in this roadmap.

## Implementation requirements

1. Build/deploy one clean telemetry-enabled ARM package.
2. Collect at least three fresh/cold-catalog runs and three warm-catalog runs under comparable network conditions; STOP if conditions are not defensible.
3. Record process start→scope ready, warm committed query ready, first useful Home frame, and full background sync completion.
4. Record CW/RA/Views, first library page, route/fallback, DB queue wait, page transaction/commit, readMediaPage, artwork overlap.
5. Record CPU average/peak where available, RSS average/sampled max/process peak, process reads/writes, frame stalls, telemetry drops/writer errors, final catalog size.
6. Compare to prior Task 33 evidence only where definitions are compatible; mark non-comparable metrics.
7. Classify remaining bottleneck by evidence: network, route fallback, SQLite commit/query, artwork contention, UI/framebuffer, or unknown.
8. Record an evidence-based decision: acceptable/no immediate optimization, or propose a separate future task. Implement nothing here.
9. State original `sqlite-migration/tasks/34-retire-legacy-catalog-cache-sync-persistence.md` remains PAUSED pending separate approval.

## Tests

- Three valid cold runs.
- Three valid warm runs.
- Required timings/resources recorded or unavailable with reason.
- Telemetry health explained.
- No production source diff.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make test -j2
make -j2
make onionos
make verify-arm
# Deploy through established OnionOS package/remote-launch workflow.
# Run >=3 cold and >=3 warm scenarios with telemetry enabled.
# Decode/analyze telemetry and record factual results.
git status --short
git diff --check
```

## Hardware gate

MANDATORY. Physical Miyoo Mini Plus evidence is required; ARM build alone does not complete the task.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.
- Physical Miyoo evidence unavailable.
- Run conditions too inconsistent for comparison.
- Required timeline stage still unmeasurable.
- Correctness regression appears.
- Production/durability change seems necessary inside benchmark task.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
perf(catalog): rebaseline sqlite refactor on Miyoo
```

Do not include any part of Task 34 in this commit.

## Checkpoint

**CP-D — Refactor rebaseline complete. STOP. Original SQLite Task 34 remains paused until explicit user review/authorization.**

Do not start the next phase until this checkpoint is explicitly approved.
