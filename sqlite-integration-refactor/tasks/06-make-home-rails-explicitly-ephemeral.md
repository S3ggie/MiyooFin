# Task 06 — Make Home rails explicitly ephemeral

**Phase:** A — correctness

## Objective

Make current Jellyfin Continue Watching/Recently Added responses the sole normal runtime authority for those Home rails; keep `home_items` compatibility-only.

## Why

SQLite has `home_items` while active Home rails are maintained separately, creating two half-authoritative representations.

## Preconditions

- Task 05 committed Movies/Shows membership is authoritative.

## Allowed Files

- `src/catalog/CatalogDb.hpp`
- `src/catalog/CatalogDb.cpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreenRefresh.cpp`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_cache_offline.inc` if compatibility coverage belongs there

## Forbidden Scope

- Do not drop `home_items` or change schema v3.
- Do not delete snapshot compatibility APIs yet.
- Do not persist rails into another store.
- Do not change Jellyfin endpoint semantics.

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

1. Document normal rails as ephemeral presentation state.
2. Normal bounded top-level sync must not write `home_items`.
3. Normal Home startup/refresh must not read `home_items` as authority.
4. `home_items` remains reachable only through snapshot compatibility/test bridge until Task 13.
5. Warm catalog may render Movies/Shows shell without rails until rail refresh completes.

## Tests

- Normal population leaves pre-seeded compatibility `home_items` untouched.
- Compatibility seed/read round-trip still passes.
- Rail failure cannot change committed Movies/Shows membership.

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
refactor(home): make home rails ephemeral
```

Do not include any part of Task 07 in this commit.
