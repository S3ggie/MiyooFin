# Task 11 — Atomic series-subtree UPSERT



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

Persist one completely fetched series subtree in a single bounded SQLite transaction.

## Depends On

- Task 10

## Allowed Files

- CatalogDb/catalog SQL/codec files
- Focused tests

## Forbidden Scope

- No top-level series deletion/reconciliation yet.
- No screen/worker consumer switch.
- No checkpoint advancement.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Model complete/incomplete fetch inputs from Home hierarchy worker.
- Confirm network caller only invokes durable write after every season/episode fetch succeeds.

## Exact implementation requirements

1. Add `UpsertSeriesHierarchy(series,seasons,episodesBySeason,generation,refreshMs)` worker job.
2. Reject empty series ID or incomplete/malformed relationship input.
3. `BEGIN IMMEDIATE` transaction.
4. UPSERT series, seasons, episodes and their genre/tag rows using cached statements.
5. Delete stale episodes for each authoritative returned season.
6. Delete stale seasons absent from authoritative series response, cascading children.
7. Mark hierarchy_state complete for that series/generation only after all row changes succeed.
8. COMMIT once per series subtree.
9. Rollback on any bind/step/cancellation/error.
10. Check cancellation at bounded row intervals but never commit a partial subtree.

## Invariants

- Series subtree atomic.
- No checkpoint advancement.
- Idempotent repeat produces same logical state.
- No per-row transaction.

## Focused tests

- New series.
- Update metadata.
- Remove episode.
- Remove season.
- Empty complete series.
- Injected step failure rollback.
- Cancellation rollback.
- Idempotent reapply.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): persist series hierarchy atomically
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Caller can only provide partial hierarchy but marks it complete.
- Cancellation handling would expose partial committed state.
- Transaction memory grows unbounded for realistic series.

Do not broaden the task to work around a STOP condition.
