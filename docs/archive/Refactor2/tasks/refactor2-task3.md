# Refactor2 Task 3 — Split API and session tests

## Objective

Split the ~1,346-line API/session case file into focused API and session binaries, preserving every case unchanged.

## Preconditions

- Tasks 1–2 commits exist.
- Baseline: `make output/test/test_api_session -j2` passes.

## Allowed Files

- `tests/test_api_session.cpp`
- `tests/cases/test_api_session.inc`
- New `tests/test_api_*.cpp`, `tests/test_session_*.cpp`
- New `tests/cases/test_api_*.inc`, `tests/cases/test_session_*.inc`
- `tests/test_runner.sh`
- `tests/test_support.hpp` or one new narrowly scoped `tests/api_session_test_support.hpp`
- `Makefile`

## Required result

- Separate session persistence/authentication lifecycle tests from Jellyfin request/response/API tests.
- If either side remains above 1,000 lines, split API tests once more by endpoint family.
- Preserve all assertions, fixture bytes, request strings, and case ordering within each resulting file.
- Do not modify production files.
- Register every new binary and explicit case dependency in `Makefile` and `tests/test_runner.sh`.

## Verification

Run each resulting API/session binary directly, then:

```sh
make test -j2
git diff --check
```

Compare test names before/after and STOP on any missing or duplicate case.

## Commit

```text
build(test): split api and session groups
```
