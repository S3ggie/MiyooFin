# Task 16 — Unify Series and EpisodeBrowser refresh policy

**Phase:** B — ownership/modularity

## Objective

Route SeriesScreen and EpisodeBrowser hierarchy reads/refreshes through LibraryQuery/LibrarySync.

## Why

Both screens independently implement DB → network → DB policy today.

## Preconditions

- Task 15 clean boundaries are present.
- Shared LibrarySync/LibraryQuery are available to screens.

## Allowed Files

- `src/library/LibrarySync.*`
- `src/library/LibraryQuery.*`
- `src/ui/screens/SeriesScreen.cpp`/`.hpp`
- `src/ui/screens/EpisodeBrowserScreen.cpp`/`.hpp`
- `src/app/ScreenStack.*` or construction call sites only if shared services must be propagated
- `src/ui/screens/HomeScreenNavigation.cpp`
- `tests/cases/test_artwork_episode.inc`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_cache_offline.inc`

## Forbidden Scope

- Do not change layout/input/artwork/playback/download-button behavior.
- Do not network from UI methods.
- LibraryQuery never performs network fallback.
- Do not alter DownloadStore availability semantics.

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

1. Add LibrarySync hierarchy refresh operations using existing cancellable Jellyfin calls and CatalogDb hierarchy writes.
2. Series reads cached seasons through LibraryQuery on background work; online refresh uses LibrarySync and preserves selection.
3. EpisodeBrowser does the same for episodes and preserves initial selection.
4. Screens no longer call Jellyfin getSeasons/getEpisodes or CatalogDb hierarchy writes directly.
5. Use OfflineLibraryQuery/shared offline helper for downloaded-only filtering/synthesis where practical without behavior change.
6. Stale-screen cancellation remains generation/scope safe.

## Tests

- Series cached result works without network and selection survives refresh.
- Episode cached result works without network and initial episode selection survives.
- Shared network refresh becomes queryable through LibraryQuery.
- Cancelled screen cannot publish stale hierarchy.
- Direct hierarchy network/write calls are absent from both screens.

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
refactor(library): unify hierarchy screen refresh
```

Do not include any part of Task 17 in this commit.
