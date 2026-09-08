# Task 038 — freeze MFT v1 and finalize architecture documentation

## Status
NOT STARTED

## Depends On
- `037`

## Goal
After successful hardware review, align repository docs with proven implementation and freeze MFT v1.

## Why This Task Exists
Schema freeze is the final step after implementation and observer-effect validation.

## Allowed Files
- `docs/architecture.md`
- `docs/performance-telemetry.md`
- `tools/telemetry/README.md`

## Forbidden Scope
- No application code.
- No MFT field changes.
- Do not rewrite benchmark evidence.

## Pre-change Checks
```sh
git rev-parse HEAD
git status --short
make test -j2
```

## Exact Steps
1. Compare telemetry/SCHEMA_V1.md against implemented encoder and desktop decoder; if any mismatch exists, STOP.
2. Document final facade→ring→service→metrics/writer→MFT→laptop architecture in docs/architecture.md.
3. State UiDiagnostics remains watchdog and PerformanceTelemetry is sibling.
4. Document proven defaults for cadence/buffering/rotation/128MiB cutoff and link benchmark evidence.
5. Perform final coverage review for all required domains and privacy/low-storage/observer-effect guarantees.

## Behavior / Invariants That Must Not Change
- Stay inside Allowed Files.
- Preserve unrelated application behavior and existing thread ownership.
- Preserve telemetry security, compile-out, and runtime-off guarantees.

## Focused Validation
```sh
make test -j2
```

## Required Final Validation
```sh
make test -j2
make -j2
git diff --check
```

## Acceptance Criteria
- Docs match encoder/decoder.
- Hardware review passed before freeze wording.
- All requirements have implementation owners.

## Commit
```sh
git add docs/architecture.md docs/performance-telemetry.md tools/telemetry/README.md
git commit -m "docs: freeze telemetry v1"
```

## STOP
After the commit, run `git status --short` and `git rev-parse HEAD`, report the commit SHA, and STOP. Do not start another numbered task. If the work cannot be completed safely inside Allowed Files, STOP without committing.
