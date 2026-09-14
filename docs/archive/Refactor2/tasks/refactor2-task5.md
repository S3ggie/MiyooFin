# Refactor2 Task 5 — Extract CatalogDb write and transaction implementation

## Objective

Move CatalogDb mutation primitives, transaction helpers, page/item upserts, deletes, and generation staging/finalization/abort definitions into `CatalogDbWrite.cpp`.

## Preconditions

- Task 4 commit exists and catalog tests pass.

## Allowed Files

- `src/catalog/CatalogDb.cpp`, `CatalogDb.hpp`, `CatalogDbInternal.hpp`, `CatalogDbSchema.cpp`
- New `src/catalog/CatalogDbWrite.cpp`
- Catalog test files
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- `tools/refactor-check.sh`

## Required result

- Group only write-side method definitions and the private helpers used solely by them.
- Preserve transaction boundaries, statement order, binding types, generation checks, cancellation checks, affected-row semantics, error propagation, and futures.
- Keep all SQLite work on the existing CatalogDb worker. No second connection or worker.
- Do not move domain synchronization policy from `LibrarySync` into CatalogDb.
- Add explicit build registrations.

## Verification

Run every catalog binary plus:

```sh
make test -j2
make -j2
make refactor-check
git diff --check
```

STOP if any SQL text or transaction boundary must change.

## Commit

```text
refactor(catalog): extract write implementation
```
