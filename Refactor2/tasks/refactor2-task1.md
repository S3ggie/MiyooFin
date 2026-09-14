# Refactor2 Task 1 — Split telemetry tests into focused binaries

## Objective

Replace the ~2,190-line telemetry case group with smaller focused binaries so later telemetry changes compile and run independently. This task changes test organization only.

## Preconditions

- Follow `Refactor2/EXECUTION_RULES.md`.
- Baseline: `make output/test/test_telemetry -j2` passes.

## Allowed Files

- `tests/test_telemetry.cpp`
- `tests/cases/test_telemetry.inc`
- New `tests/test_telemetry_*.cpp` wrappers
- New `tests/cases/test_telemetry_*.inc` case files
- `tests/test_runner.sh`
- `Makefile`

## Required result

- Divide cases by existing concern: encoding/format, recorder/state counters, service/rotation, and integration/schema behavior. Use the current case names to choose the exact boundary.
- Every resulting `.inc` file must be under 1,000 lines; target 350–750 lines.
- Each wrapper has its own `main`, includes only `test_support.hpp` and its assigned case file(s), and preserves existing test registration conventions.
- Every original test function/case appears exactly once. Preserve assertion bodies and execution order within each concern.
- Register every new binary in `TEST_GROUPS`, dependencies, and `tests/test_runner.sh`. Remove obsolete registration only after all cases moved.

## Verification

Run:

```sh
make output/test/test_telemetry -j2
make output/test/test_telemetry_format output/test/test_telemetry_service -j2
make test -j2
git diff --check
```

If chosen binary names differ, substitute the exact new names in the second command and record them in the commit message body.

Also compare the before/after list of test case names. STOP if any case is missing, duplicated, or behavior was edited.

## Commit

Stage only Allowed Files and commit:

```text
build(test): split telemetry test groups
```
