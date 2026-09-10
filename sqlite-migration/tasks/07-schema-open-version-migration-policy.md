# Task 07 — Schema open/version/migration policy



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- After the successful task commit, **STOP for mandatory human review: CP-A — SQLite build/core/scope/schema**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Implement supported-open and schema-version handling so future migrations are transactional and future unknown versions are never modified.

## Depends On

- Task 06

## Allowed Files

- `src/catalog/CatalogDb.*`
- `src/catalog/CatalogSchema.*`
- Focused tests

## Forbidden Scope

- No actual schema v2.
- No legacy OfflineCatalog import.
- No consumer cutover.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate open states: missing DB, v1 DB, wrong app ID, user_version 0, future version, corrupt header.

## Exact implementation requirements

1. New missing DB -> create v1.
2. Known application_id + v1 -> open normally.
3. Wrong nonzero application_id -> reject without modification.
4. Future user_version -> reject without modification.
5. Define migration helper contract: finalize statement cache, BEGIN IMMEDIATE, migrate, set user_version, COMMIT, rebuild statements.
6. Rollback on migration failure.
7. Do not silently delete/rebuild a future-version DB.
8. Make open result distinguish missing/new, supported, unsupported-version, wrong-app, corruption/I/O.

## Invariants

- No destructive open fallback.
- Schema migration is transactional.
- Statement cache never survives schema mutation.

## Focused tests

- All open-state fixtures.
- Failed migration rollback simulation.
- Future-version byte/hash unchanged after rejected open.
- Rebuild ARM executable/package and report exact post-Task-07 byte sizes alongside Task-01 pre-SQLite baseline.
- If practical in the available environment, measure idle RSS before vs after CatalogDb service initialization with no scoped DB workload; use Miyoo telemetry when actual hardware is available, otherwise a controlled host comparison. If a meaningful comparison is not practical, report `NOT PERFORMED` with the reason rather than inventing a result.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
make package
stat -c %s output/build-arm/miyoofin
du -sb output/package/MiyooFin | awk '{print $1}'
# If practical, capture idle RSS before vs after CatalogDb service initialization without DB workload
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): enforce catalog schema version policy
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- SQLite open behavior would modify an unsupported DB before validation.
- A future-version DB cannot be detected safely.
- Any migration path requires nontransactional destructive steps.

Do not broaden the task to work around a STOP condition.
