# Task 17 — Unify DownloadManager hierarchy policy

**Phase:** B — ownership/modularity

## Objective

Make DownloadManager planning obtain hierarchy through LibraryQuery/LibrarySync instead of a duplicate DB → network → DB loop.

## Why

The download planner duplicates the same hierarchy acquisition policy as screens.

## Preconditions

- Task 16 shared hierarchy policy is working.

## Allowed Files

- `src/download/DownloadManager.hpp`
- `src/download/DownloadManager.cpp`
- `src/download/DownloadManagerPlanning.cpp`
- `src/library/LibraryQuery.*` only for narrow planner-facing additions
- `src/library/LibrarySync.*` only for narrow refresh additions
- `src/app/App.cpp`/`.hpp` wiring
- `tests/cases/test_downloads.inc`
- `tests/cases/test_catalog_parity.inc`

## Forbidden Scope

- Do not change HLS/transfer/retry/.part/size/pause/resume/delete/manifests.
- Do not block SDL thread.
- Do not change media-source preflight except hierarchy acquisition.

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

1. Wire shared LibraryQuery/LibrarySync into planner jobs instead of raw CatalogDb for hierarchy.
2. Season plans query episodes; missing/incomplete hierarchy requests shared refresh then re-queries/uses published result.
3. Series plans query seasons+episodes; incomplete hierarchy uses one shared LibrarySync path.
4. Remove direct hierarchy Jellyfin calls and CatalogDb hierarchy writes from DownloadManager planning.
5. Keep download-specific media-source discovery and plan calculation in DownloadManager.
6. Preserve planner generation/session supersession.

## Tests

- Fully cached plan causes no hierarchy network call.
- Missing hierarchy refreshes through LibrarySync and includes all expected episodes.
- Superseded planner ignores old result.
- Existing size/canFit/dedup behavior unchanged.
- Duplicate hierarchy fallback code is absent from DownloadManagerPlanning.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
```

## Hardware gate

None. This task is host/ARM-build validation only; do not deploy to a Miyoo Mini Plus.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
refactor(download): share hierarchy sync policy
```

Do not include any part of Task 18 in this commit.

## Checkpoint

**CP-B — Ownership/modularity. STOP after this task and obtain explicit architecture approval before Phase C.**

Do not start the next phase until this checkpoint is explicitly approved.
