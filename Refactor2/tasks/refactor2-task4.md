# Refactor2 Task 4 — Extract CatalogDb schema and migration implementation

## Objective

Move CatalogDb schema creation, schema-version handling, migrations, database bootstrap/open, integrity setup, and related file-promotion helpers out of `CatalogDb.cpp` into `CatalogDbSchema.cpp`. Preserve the `CatalogDb` public interface and exact SQL.

## Preconditions

- Tasks 1–3 commits exist.
- Baseline: run all catalog test binaries created by Task 2.

## Allowed Files

- `src/catalog/CatalogDb.cpp`
- `src/catalog/CatalogDb.hpp`
- New `src/catalog/CatalogDbSchema.cpp`
- A new private `src/catalog/CatalogDbInternal.hpp` only if file-local helpers must be shared
- Catalog tests and support files
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- `tools/refactor-check.sh`

## Required result

- Move existing definitions; do not redesign the class.
- Keep schema version, table/index definitions, migration order, pragmas, temporary fresh-database path, atomic promotion, rollback, and corruption/failure behavior byte-for-byte equivalent.
- Do not expose SQLite in a new public header. `CatalogDbInternal.hpp`, if needed, is catalog-private and contains no networking/UI/download/cache dependencies.
- Leave worker startup/shutdown and queue ownership in `CatalogDb.cpp`.
- Add the new source to all applicable explicit source lists.

## Tests and verification

Add no new behavior tests unless an extraction reveals an uncovered existing branch; any added test must assert current behavior. Run all catalog binaries, then:

```sh
make test -j2
make -j2
make refactor-check
git diff --check
```

STOP if extraction requires SQL, schema, bootstrap, or migration behavior changes.

## Commit

```text
refactor(catalog): extract schema implementation
```
