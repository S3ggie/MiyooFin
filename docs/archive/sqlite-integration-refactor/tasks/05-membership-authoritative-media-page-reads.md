# Task 05 — Make top-level media reads membership-authoritative

**Phase:** A — correctness

## Objective

Make Movies/Shows page reads return only media currently referenced by committed supported library membership.

## Why

`readMediaPage()` currently selects historical rows by media kind and only looks up membership afterward.

## Preconditions

- Task 04 authoritative finalize is present.

## Allowed Files

- `src/catalog/CatalogDb.cpp`
- `src/catalog/CatalogDb.hpp` only for a narrow result change
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_migration.inc` only for query-plan checks

## Forbidden Scope

- Do not delete historical media rows as the fix.
- Do not remove keyset paging/sort indexes.
- Do not change Anime UI classification yet.
- Do not add a read connection.

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

1. Media page SQL must require at least one committed membership joined to a committed supported view.
2. Movies require a `movies` view; shows require a `tvshows` view. Playlist-only membership is not browse-authoritative.
3. Deduplicate an item across multiple supported views while preserving keyset order `(organizational_sort_key,title,id)`.
4. Return all current supported memberships for each returned item.
5. Keep page limits/cursors bounded and query-plan coverage using existing sort indexes.
6. Unreferenced historical media rows must not appear.

## Tests

- Historical row with zero committed membership is absent.
- Item in two supported views appears once with both memberships.
- Wrong-type/Playlist-only memberships are excluded.
- Keyset pages remain stable and duplicate-free.
- Query plan still avoids a temp full sort.

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
fix(catalog): require current membership for media pages
```

Do not include any part of Task 06 in this commit.
