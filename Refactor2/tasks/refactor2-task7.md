# Refactor2 Task 7 — Extract CatalogDb sync-state and hierarchy implementation

## Objective

Finish decomposing CatalogDb by moving sync checkpoint/state persistence and series/season/episode hierarchy primitives into concern-specific translation units.

## Preconditions

- Task 6 commit exists and catalog tests pass.

## Allowed Files

- All `src/catalog/CatalogDb*` files
- New `src/catalog/CatalogDbSyncState.cpp`
- New `src/catalog/CatalogDbHierarchy.cpp`
- Catalog and cache/offline test files
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- `tools/refactor-check.sh`
- `docs/architecture.md`

## Required result

- `CatalogDb.cpp` retains construction/destruction, worker/queue lifecycle, scope configuration, task dispatch, and genuinely shared core helpers.
- `CatalogDbSyncState.cpp` owns existing durable checkpoint/sync-state method definitions.
- `CatalogDbHierarchy.cpp` owns existing hierarchy persistence/query primitives.
- Preserve SQL, public signatures, futures, worker serialization, checkpoint monotonicity, hierarchy membership, and cancellation semantics.
- Aim for `CatalogDb.cpp` below 1,500 lines. If it remains larger, document why in the commit body; do not invent another boundary.
- Update the architecture source map only for completed extractions.

## Verification

Run catalog, cache/offline, artwork/episode, and download binaries, then:

```sh
make test -j2
make -j2
make refactor-check
git diff --check
```

## Commit

```text
refactor(catalog): isolate sync and hierarchy primitives
```
