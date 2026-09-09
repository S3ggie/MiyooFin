# Execution Rules — v2

These rules apply to every numbered file under `sqlite-migration/tasks/`.

## 1. Direct execution override for this roadmap

The user explicitly overrides the repository `AGENTS.md` delegation preference for these numbered SQLite tasks:

- the main/current Codex model performs implementation directly;
- use the currently selected GPT-5.6 Luna High;
- do not spawn implementation subagents;
- do not spawn reviewer subagents by default;
- do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.

This override is limited to delegation/orchestration. All repository safety, dirty-work, local-first, threading, hardware-evidence, and validation requirements still apply.

## 2. Autonomous task flow

One numbered task equals one commit.

For every task:

1. read current `AGENTS.md`, this file, the task, dependencies, and last checkpoint decision;
2. run HEAD/status checks;
3. implement directly within Allowed Files;
4. run all focused and complete validation commands;
5. if successful, create exactly one commit using the task's required commit message;
6. in autonomous roadmap mode, immediately continue to the next numbered ordinary task;
7. STOP at mandatory human checkpoints or genuine STOP conditions.

The task file's commit section is explicit user authorization for that task's one commit. No separate "may I commit?" confirmation is needed.

Do not combine tasks into one commit.

If a task fails validation or hits a STOP condition, do not commit a false completion.

## 3. Mandatory checkpoints

Stop after Tasks:

- 07 -> CP-A
- 16 -> CP-B
- 18 -> CP-C
- 20 -> CP-D
- 26 -> CP-E
- 27 -> CP-F
- 33 -> CP-G
- 34 -> CP-H

Do not cross a checkpoint without explicit human approval.

## 4. Git safety

Before every task:

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

Preserve unrelated dirty work. Never reset, clean, discard, or overwrite unrelated user changes.

The task commit must contain only that task's allowed changes.

## 5. Scope safety

CatalogDb is app-scoped but DB-scoped per server/user.

- no DB open before a valid scope;
- use existing LibraryCache scope derivation;
- every scoped job/result carries a scope epoch;
- configure/deconfigure advances requested epoch immediately;
- stale old-epoch results never publish;
- worker finalizes/closes old DB before opening new;
- logout leaves CatalogDb unconfigured;
- never expose previous account/server rows after a switch.

## 6. No SQLite on the SDL/UI thread

Never open, query, prepare, step, reset, finalize, transact, integrity-check, checkpoint, migrate, or synchronously wait for SQLite from:

- screen construction/enter;
- handleAction;
- update;
- render;
- App's frame loop.

Only CatalogDb's owned worker calls SQLite.

## 7. One connection and bounded work

Initial architecture:

- one worker;
- one SQLite connection;
- bounded priority queue;
- prepared statement reuse;
- bounded result vectors;
- no detached threads.

Do not add reader connections without a separately approved evidence-driven architecture change.

## 8. Migration safety

Legacy `catalog.v1` is immutable migration input.

Normal migration activation, once Task 17 lands, is the worker-side `configureScope` path. It is not a hidden diagnostic.

Before consumer cutover, successful SQLite import/open does not by itself make SQLite the production read authority.

No production dual-write.

## 9. MFT v1 freeze

`telemetry/SCHEMA_V1.md` is frozen.

Do not alter v1 record layouts/enums/meanings.

Task 13 creates explicit MFT v2 and dual-version decoder compatibility.

## 10. Hardware evidence

Tasks 18, 20, 27, and 33 require actual Miyoo Mini Plus evidence.

ARM compilation is not hardware runtime evidence.

Successful hardware tasks update factual `docs/sqlite-migration-benchmark.md` so the task has a reviewable commit. This is benchmark/product evidence, not an orchestration ledger.

Do not patch application code inside a hardware-evidence task. If hardware exposes a bug requiring code, STOP and add/review a focused task before rerunning the hardware gate.

## 11. Footprint evidence at CP-A

Record exact ARM executable and packaged application size:

- clean pre-SQLite baseline before Task 01 changes;
- post-core/schema after Task 07.

Also measure idle RSS before vs after CatalogDb service initialization if practical. If it cannot be measured meaningfully, report `NOT PERFORMED` and why.

## 12. STOP means stop

A STOP condition is not permission for broad cleanup/refactoring.

Preserve evidence, report the blocker, and wait for human review.
