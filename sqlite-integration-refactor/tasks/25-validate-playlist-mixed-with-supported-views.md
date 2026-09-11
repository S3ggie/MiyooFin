# Task 25 — Validate Playlist mixed with supported views

**Phase:** C — validation

## Objective

Prove unsupported Playlist views are skipped without truncating later supported libraries or making playlist-only items browse-authoritative.

## Why

Unsupported view handling must not recreate the break/flag coupling seen with optional Home rails.

## Preconditions

- Task 24 passes.

## Allowed Files

- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_api_session.inc` if supported-view filtering seam lives there

## Forbidden Scope

- Do not patch production behavior inside this validation task. If the scenario fails and needs code, STOP and create/review a focused corrective task before rerunning this validation task.
- Do not weaken assertions to match incorrect output.
- Do not change playback repair code.

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

1. Feed view order Movies → Playlist → TV.
2. Run synchronization through all views/pages.
3. Include Playlist-only item and one supported item also referenced by Playlist.

## Tests

- TV after Playlist still synced.
- Playlist-only item absent from Movies/Shows.
- Supported item reachable only via supported membership.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
git diff --check
```

## Hardware gate

None. This task is deterministic host validation; ARM runtime is covered by Tasks 30-31.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.
- Any assertion exposes a production correctness defect that cannot be explained by a bad fixture/test seam.
- The test requires broadening into production files outside this task's validation scope.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
test(library): cover unsupported playlist interleave
```

Do not include any part of Task 26 in this commit.
