# SQLite Integration Status

**Status: COMPLETE.** The SQLite integration work is finished and SQLite is the
persistence and query authority for the catalog. A small legacy compatibility
surface is retained deliberately (see [Persistence authority](#persistence-authority)).

This is the single sign-off record for the SQLite work so that the archived
roadmaps can be considered closed and future work can proceed without
revisiting them.

## Scope

This record covers the original `sqlite-migration` roadmap and the follow-up
`sqlite-integration-refactor` roadmap (tasks 01–40), both now under
`docs/archive/`.

## Checkpoints

| Checkpoint | Meaning | Status | Evidence |
|---|---|---|---|
| CP-A | Correctness foundation (failure isolation, authoritative generations, warm reads, scope ownership, offline roots) | Done | Tasks 01–09; host suite |
| CP-B | Ownership/modularity (LibrarySync/LibraryQuery owners; CatalogDb narrowed) | Done | Tasks 10–17; `tools/refactor-check.sh` |
| CP-C | Hardware correctness on a physical Miyoo Mini Plus | **Partially evidenced (exploratory)** | `docs/sqlite-integration-refactor-validation.md` |
| CP-D | Performance rebaseline on real hardware | **COMPLETE — six valid physical-device runs** | `docs/sqlite-integration-refactor-benchmark.md` |
| CP-E | Incremental synchronization (live changes, catch-up, safety reconcile) | Implemented | Tasks 35–40; commits below |

The `sqlite-integration-refactor` roadmap defines completion as "Task 33 and
CP-D complete". CP-D is recorded complete, so the roadmap is satisfied by its
own definition.

CP-C is recorded honestly: the device validation was an exploratory
current-build check, not the full destructive fresh-data matrix. The scenarios
it did not instrument are listed under
[Optional follow-ups](#optional-follow-ups-not-required).

## Persistence authority

Decision (2026-09-14): **SQLite (`CatalogDb`, schema v3) is the persistence and
query authority.** `LibrarySync` owns Jellyfin → SQLite synchronization;
`LibraryQuery` owns domain reads; `DownloadStore` remains authoritative for
physical offline availability. Legacy whole-file snapshot persistence is retired
at runtime (commit `d84f8b3 refactor(catalog): retire legacy whole-file
persistence`) and has no runtime caller.

The following residual compatibility surface is **retained by decision**, not
because it is required for correctness:

| Item | Role |
|---|---|
| `src/catalog/CatalogCompatibility.*` | Bridge used by `CatalogDb` to seed/read `LibrarySnapshot` |
| `src/cache/LibraryCache.*` (`load`/`save` codec) | Back-compat codec, exercised by tests only |
| `src/cache/OfflineCatalog.hpp` | `OfflineCatalogSnapshot` metadata struct only |
| `LibrarySnapshot` / `CachedLibraryView` types | Data types used by the bridge |

Rationale: this is roughly one hundred lines across a few small, already
isolated files. Removing it buys no functional benefit and would touch the
offline path, which is hardware-validated. The substantive goal of the original
"retire legacy catalog cache/sync persistence" task (removing whole-file
persistence) is already satisfied. Retiring the bridge remains an explicitly
optional future cleanup, not an open correctness item.

## Evidence

- `docs/archive/sqlite-integration-refactor/README.md` — roadmap, ownership
  target, and checkpoint definitions.
- `docs/sqlite-integration-refactor-benchmark.md` — CP-D rebaseline, six
  physical-device runs, 2026-09-13.
- `docs/sqlite-integration-refactor-validation.md` — exploratory device
  validation, 2026-09-13.
- `docs/sqlite-migration-benchmark.md` — pre-refactor baseline.
- Commit `d84f8b3` — legacy whole-file persistence retired.
- Commit `0b8a1f0` — original migration roadmap removed as completed.
- Phase E commits: `f569a19`, `3b07ad4`, `e1d1a1d`, `af43b12`, `8cd9ca6`.

## Host validation

Run from the repository root on the recorded tree and passing:

```sh
make -j2
make test -j2
sh tools/refactor-check.sh
git diff --check
make onionos
make verify-arm
```

A physical-device smoke test also passed on the refactored build (Home, Movies,
Shows, Anime, artwork, navigation, and graceful exit).

## Optional follow-ups (not required)

- Complete a full CP-C destructive device matrix: fresh database, multiple TV
  libraries, offline-without-LibraryCache, and exit-during-synchronization.
- A dedicated future task to remove the residual compatibility bridge, if it is
  ever worth the risk.
