# Refactor2 Task 6 — Extract CatalogDb query implementation

## Objective

Move bounded media reads, lookup/query methods, count/window/cursor reads, and read-only row decoding into `CatalogDbQuery.cpp`.

## Preconditions

- Task 5 commit exists and catalog tests pass.

## Allowed Files

- `src/catalog/CatalogDb.cpp`, `CatalogDb.hpp`, `CatalogDbInternal.hpp`
- `src/catalog/CatalogDbSchema.cpp`, `CatalogDbWrite.cpp`, `MediaItemSql.*`
- New `src/catalog/CatalogDbQuery.cpp`
- Catalog and cache/offline test files
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- `tools/refactor-check.sh`

## Required result

- Move read-side definitions and read-only private helpers; preserve public signatures.
- Preserve exact SQL, ordering, collation, filtering, cursor/window behavior, limits, null/default decoding, and result/error semantics.
- Do not materialize the whole library or bypass `LibraryQuery`.
- Keep reads on the existing worker/connection.
- Do not merge `MediaItemSql` into the new file.

## Verification

Run catalog and cache/offline binaries, then:

```sh
make test -j2
make -j2
make refactor-check
git diff --check
```

STOP if the extraction requires changing a query or observable ordering.

## Commit

```text
refactor(catalog): extract query implementation
```
