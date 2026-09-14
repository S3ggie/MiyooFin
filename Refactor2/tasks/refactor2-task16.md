# Refactor2 Task 16 — Enforce boundaries and document final architecture

## Objective

Audit the completed Refactor2 state, add narrow dependency checks for the new boundaries, synchronize architecture/build documentation, and run the complete host validation. This task must not perform another production refactor.

## Preconditions

- Tasks 1–15 exact commit subjects exist in order.
- Worktree is clean.

## Allowed Files

- `tools/refactor-check.sh`
- `docs/architecture.md`
- `Refactor2/README.md`
- `Refactor2/EXECUTION_RULES.md`
- `Makefile`, `Makefile.cross`, `Makefile.desktop`
- Test runner/wrapper files only to correct registration discovered by the audit
- Production files only for include-list/build-registration corrections; no method-body changes

## Required audit

1. Confirm all test case files are under the repository's roughly 1,000-line threshold or document a justified exception in `docs/architecture.md`.
2. Confirm every new production `.cpp` is present in all applicable explicit host, cross, desktop, and test source lists, with no duplicates.
3. Confirm `CatalogDb.cpp` is core worker/lifecycle coordination and new CatalogDb units contain the intended method families.
4. Confirm `LibrarySync` remains the only Jellyfin-to-SQLite synchronization owner.
5. Confirm screen core files retain lifecycle/coordination and concern files contain render/data/worker work.
6. Confirm no production `.cpp` is included by a test.
7. Extend `tools/refactor-check.sh` with narrow, stable checks only: prohibited upward dependencies in catalog-private files; no networking headers in render units; and presence of required modular source registrations. Do not add a generic linter or fragile line-count gate.
8. Update `docs/architecture.md` source map and responsibility text to match actual final files. Mark all task links complete in `Refactor2/README.md` only after validation.

## Complete verification

Run:

```sh
make refactor-check
make test -j2
make -j2
make bridge bridge-test reporter reporter-test -j2
sh -n distributions/onionos/playback_runner.sh
git diff --check
git status --short
```

Do not run `make onionos`, `make verify-arm`, or deploy: these refactors are host-validated only. STOP if complete validation exposes a production defect; do not hide it with a broad fix in this audit task.

## Commit

```text
docs(architecture): lock refactor2 module boundaries
```

After committing, provide the final roadmap execution report required by `Refactor2/EXECUTION_RULES.md`. Do not push.
