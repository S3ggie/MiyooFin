# Task 12 — Authoritative hierarchy delete/reconciliation DAL



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

Replace `OfflineCatalog::reconcileSeries` with an authoritative top-level series reconciliation transaction that never touches downloads.

## Depends On

- Task 11

## Allowed Files

- CatalogDb/catalog SQL files
- Focused tests

## Forbidden Scope

- No DownloadStore deletion.
- No Home caller switch yet.
- No checkpoint logic.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace current authoritative top-level library listing semantics.
- Confirm series IDs list is complete only after a successful library fetch.

## Exact implementation requirements

1. Add reconciliation job accepting authoritative current series IDs/metadata set.
2. Only run deletion when caller marks input authoritative/successful.
3. UPSERT top-level series metadata as needed.
4. Delete catalog series absent from authoritative set; cascade hierarchy rows.
5. Never inspect/delete downloaded bytes or DownloadStore records.
6. One bounded transaction for the reconciliation set; if list can exceed a safe bound, design deterministic chunking that does not expose false deletion before full authoritative membership is known.
7. Record reconciliation result counts for later telemetry.

## Invariants

- Network failure never causes deletion.
- Downloads survive catalog deletion.
- No checkpoint advancement.

## Focused tests

- Remove one series.
- No-op reconciliation.
- Failed/non-authoritative input refuses deletion.
- Downloaded IDs fixture unaffected.
- Rollback on injected error.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): reconcile authoritative series in SQLite
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Top-level caller cannot prove listing completeness.
- Reconciliation would require DownloadManager ownership changes.

Do not broaden the task to work around a STOP condition.
