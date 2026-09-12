# Task 17a — Route Home hierarchy through library policy

Phase B corrective task for the CP-B ownership blocker. This task is not
Phase C and must be completed before Task 18 may begin.

## Objective

Route Home hierarchy reads and refreshes through the same ownership model used
by Series/Episode and DownloadManager:

- `LibraryQuery` owns cached Home hierarchy reads.
- `LibrarySync` owns Jellyfin → SQLite hierarchy refresh and checkpoint policy.
- `CatalogDb` remains the SQLite execution boundary.
- Home retains selection/presentation, cancellation, generation supersession,
  local-first behavior, and artwork scheduling.

Home hierarchy production code must not directly call `getSeasons`,
`getEpisodes`, submit generic hierarchy persistence, or implement a generic
Jellyfin → SQLite hierarchy policy.

## Allowed Files

- `sqlite-integration-refactor/tasks/17a-route-home-hierarchy-through-library-policy.md`
- `src/ui/screens/HomeScreenHierarchy.cpp`
- `src/ui/screens/HomeScreen.cpp`
- `src/ui/screens/HomeScreen.hpp`
- `src/library/LibrarySync.cpp`
- `src/library/LibrarySync.hpp`
- `tests/cases/test_catalog_parity.inc`
- `tests/cases/test_catalog_migration.inc`

Do not modify any other repository file. Preserve existing download transfer
behavior and all unrelated user work.

## Implementation requirements

1. Add focused regression coverage proving Home hierarchy production code has no
   direct seasons/episodes Jellyfin calls or generic CatalogDb hierarchy
   persistence, and uses `LibraryQuery`/`LibrarySync` instead.
2. Move Home cached hierarchy reads to `LibraryQuery`.
3. Move Home Jellyfin hierarchy refresh and hierarchy/checkpoint writes to
   `LibrarySync`, using its existing cancellable worker-owned primitives.
4. Preserve cached availability, refresh success, transient failure behavior,
   cancellation, generation/scope supersession, loading/publishing, and
   artwork behavior.
5. Keep one CatalogDb worker/connection, schema v3, bounded work, and all
   SQLite/network work off the SDL/UI thread.

## Focused tests

- Home hierarchy ownership/source regression.
- Existing Home hierarchy integration behavior, including complete writes,
  incomplete-write rejection, authoritative replacement, stale scope
  rejection, and checkpoint preservation.
- Existing cached Home hierarchy, refresh, transient failure, and
  cancellation/supersession coverage must remain passing.

## Validation

```sh
make output/test/test_runner -j2 && output/test/test_runner
make test -j2
make -j2
make onionos
make verify-arm
make refactor-check
git diff --check
```

If a required production change falls outside Allowed Files, or if preserving
hardware-verified download behavior or the SQLite/UI-thread invariants requires
broader scope, stop without committing.

## Commit

Create exactly one commit with:

```text
refactor(home): share hierarchy sync policy
```

Do not amend Task 17 or include `AGENTS.md`.

## Completion gate

After the commit, reindex the repository under `project:miyoofin`, verify FTS5
search health, and rerun the read-only CP-B ownership review. Do not begin Task
18 or any Phase C work in this task.
