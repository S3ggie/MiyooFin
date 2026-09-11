# Task 12 — Make Home consume LibrarySync and LibraryQuery

**Phase:** B — ownership/modularity

## Objective

Remove DB synchronization/query orchestration from HomeScreen so it owns presentation/navigation only.

## Why

HomeScreen is the principal architectural knot identified by the audit.

## Preconditions

- Tasks 10-11 are committed.

## Allowed Files

- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `src/ui/screens/HomeScreenSync.cpp`
- `src/ui/screens/HomeScreenHierarchy.cpp` only to remove top-level duties
- `src/ui/screens/HomeScreenRefresh.cpp` if rail polling moves
- `src/app/App.cpp`/`.hpp`
- `src/library/LibrarySync.*` and `LibraryQuery.*` only for narrow consumer API adjustments
- `tests/cases/test_ui_foundation.inc`
- `tests/cases/test_catalog_parity.inc`

## Forbidden Scope

- Do not redesign Home layout/navigation/artwork.
- Do not move Series/Episode hierarchy policy yet.
- Do not delete legacy cache files/APIs.
- Do not mutate UI from sync/DB worker callbacks.

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

1. App passes shared LibrarySync/LibraryQuery to Home.
2. Delete Home top-level fetch thread/state and direct CatalogDb page-write/finalize calls that moved to LibrarySync.
3. Replace direct CatalogDb media reads with LibraryQuery bounded pages.
4. UI update only polls ready futures/status; never blocking `.get()` unless readiness was already established.
5. Home retains bounded windows, navigation, rendering, artwork, settings, download presentation.
6. Warm SQLite publication still precedes network refresh.
7. Sync failure after warm publication does not replace valid content with fatal loading.

## Tests

- Source-level test/seam shows no getViews/getLibraryItemsPage or top-level DB-write/generation calls in Home.
- Warm Home remains usable through sync failure.
- Fresh Home transitions after first committed generation.
- Tab/filter paging stays bounded via LibraryQuery.

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
refactor(home): consume library sync and query
```

Do not include any part of Task 13 in this commit.
