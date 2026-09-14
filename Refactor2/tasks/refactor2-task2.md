# Refactor2 Task 2 — Split catalog parity tests

## Objective

Split `tests/cases/test_catalog_parity.inc` (~2,025 lines) into independently compiled catalog parity groups without changing production code or assertions.

## Preconditions

- Task 1 commit exists.
- Baseline: `make output/test/test_catalog -j2` passes.

## Allowed Files

- `tests/test_catalog.cpp`
- `tests/cases/test_catalog_parity.inc`
- New `tests/test_catalog_*.cpp`
- New `tests/cases/test_catalog_parity_*.inc`
- `tests/test_runner.sh`
- `Makefile`

## Required result

- Partition by durable concern visible in existing cases: bounded/query parity, hierarchy/membership parity, and synchronization/change parity.
- Keep `test_catalog_core.inc` and `test_catalog_migration.inc` in the existing catalog binary unless moving them is necessary to resolve duplicate fixture ownership.
- Each new parity case file must be below 1,000 lines.
- Move cases without rewriting assertions, SQL expectations, fixture data, or order within a concern.
- Every original case appears exactly once and every new binary is registered in both Makefile dependency lists and `tests/test_runner.sh`.

## Verification

Run every resulting `output/test/test_catalog*` binary directly, then:

```sh
make test -j2
git diff --check
```

STOP if symbol duplication would require including production `.cpp` files or moving shared fixtures anywhere other than `tests/test_support.hpp` or a narrowly named catalog support header.

## Commit

```text
build(test): split catalog parity groups
```
