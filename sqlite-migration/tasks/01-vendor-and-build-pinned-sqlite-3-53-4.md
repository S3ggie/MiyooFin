# Task 01 — Vendor and build pinned SQLite 3.53.4



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- In autonomous roadmap mode, after successful validation and the task commit, immediately open the next numbered task and continue without asking for routine confirmation.

## Goal

Vendor the official SQLite 3.53.4 amalgamation and make host/ARM/test builds compile the same `sqlite3.c` as a separate C object, with zero MiyooFin runtime behavior change.

## Depends On

- None

## Allowed Files

- `vendor/sqlite/sqlite3.c`
- `vendor/sqlite/sqlite3.h`
- `vendor/sqlite/README.md` (provenance/checksums)
- `Makefile`
- `Makefile.cross`
- `THIRD_PARTY_NOTICES.md` if needed for provenance/public-domain notice

## Forbidden Scope

- No CatalogDb code.
- No schema or database files.
- No application call site may include/use SQLite yet.
- No system SQLite dependency or `-lsqlite3`.
- No Docker/toolchain package dependency on distro SQLite.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Verify `main`/intended optimization base and clean/understood status.
- Inspect current host/test/ARM source-object rules.
- Verify official archive SHA3-256 and `sqlite3.c` SHA3-256 against `SQLITE_CONFIGURATION.md`.
- Before modifying build files, produce a clean **pre-SQLite ARM baseline** and record exact `miyoofin` executable bytes and packaged MiyooFin directory/archive bytes using the repository's normal package target/outputs. Preserve these numbers in the task report/commit body for CP-A comparison; do not create a roadmap ledger file.

## Exact implementation requirements

1. Vendor exactly SQLite 3.53.4 (`3530400`) from the official amalgamation archive.
2. Record official archive and `sqlite3.c` SHA3-256 values in `vendor/sqlite/README.md`.
3. Compile `sqlite3.c` with `CC` and `-Os`, separately from MiyooFin C++ flags.
4. Apply exactly: `SQLITE_THREADSAFE=2`, `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_DQS=0`, `SQLITE_TRUSTED_SCHEMA=0`, `SQLITE_OMIT_LOAD_EXTENSION`.
5. Use the exact same vendored source for host, tests, and ARM.
6. Link the object into test/application binaries only as a dormant dependency; do not open a DB.
7. Add a tiny host test/assertion for `sqlite3_libversion_number()==3053004`, `sqlite3_threadsafe()==2`, and expected compile options.
8. Keep existing application behavior and packaging otherwise unchanged.
9. After vendoring/linking, rebuild ARM/package and record the same executable/package byte measurements for an initial size delta. CP-A will repeat/confirm the final post-core/schema numbers after Task 07.

## Invariants

- No runtime SQLite I/O.
- No speculative OMIT flags.
- No system SQLite linkage.
- Normal telemetry behavior unchanged.

## Focused tests

- Version/compile-option test.
- Host link succeeds.
- ARM link succeeds and remains ARM executable.

## Complete validation commands

```sh
make clean
make test -j2
make -j2
make onionos
make verify-arm
make package
stat -c %s output/build-arm/miyoofin
du -sb output/package/MiyooFin | awk '{print $1}'
git diff --check
git status --short
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
build(sqlite): vendor pinned 3.53.4 amalgamation
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Official checksums do not match.
- SQLite requires an unexpected target library/package.
- ARM link cannot use the same vendored source.
- Any app behavior must change to make the build work.

Do not broaden the task to work around a STOP condition.
