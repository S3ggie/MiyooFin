# Task 18 — Real Miyoo migration validation

**HARDWARE REQUIRED — this task cannot be completed without actual Miyoo Mini Plus evidence.**

## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- After the successful task commit, **STOP for mandatory human review: CP-C — Real-device migration**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Prove the one-time legacy import is safe and practical on an actual Miyoo Mini Plus SD-card filesystem.

## Depends On

- Task 17
- CP-B — Migration/parity approved

## Allowed Files

- `docs/sqlite-migration-benchmark.md` — create/update only with factual real-device migration evidence for the exact tested commit.
- No application/source changes in this hardware-validation task.
- If code fix is needed, stop and create a separate reviewed patch before repeating hardware task.

## Forbidden Scope

- No journal-mode decision yet.
- No consumer cutover.
- No deletion of user's legacy catalog.
- No hardware completion claim from Docker/host.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git diff --check
git status --short
```

- Build clean ARM package.
- Back up or preserve real legacy catalog.
- Record commit, SD free space, legacy size/counts, OnionOS/firmware if available.
- Enable telemetry per benchmark protocol.

## Exact implementation requirements

1. Trigger migration through the **normal Task-17 activation path**: launch MiyooFin normally with a valid saved session (or perform a normal successful login) for a scope that has legacy `catalog.v1` and no final SQLite DB. Do not invoke a hidden diagnostic-only migration command.
2. Capture migration wall/monotonic duration, CPU, RSS/peak, process reads/writes, final DB size.
3. Reopen after clean app restart.
4. Validate quick_check/FK check through safe diagnostic tooling.
5. Simulate interrupted migration with process termination and verify restart discards/rebuilds temp while legacy remains intact.
6. Verify final DB does not become authoritative from partial temp.
7. Record the tested commit, normal activation method, legacy/final state, measured values, and recovery result in `docs/sqlite-migration-benchmark.md`. This benchmark result document is product evidence, not an orchestration ledger. If the run was not performed, STOP and do not create the success commit.

## Invariants

- Legacy untouched.
- Downloads unaffected.
- Real hardware only.

## Focused tests

- Successful real import.
- Process-killed import recovery.
- Clean restart/reopen.

## Complete validation commands

```sh
make onionos
make verify-arm
# deploy using the repository's normal approved deployment workflow
# launch via distributions/onionos/launch.sh / normal Onion launcher
# collect and decode MFT telemetry with existing tools
# run the task's safe catalog validation command/tool on-device
git diff --check
git status --short
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
test(catalog): validate SQLite migration on Miyoo hardware
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- No physical Miyoo access.
- Legacy source changes.
- Migration produces corruption/FK mismatch.
- Process-kill recovery fails.
- Evidence cannot identify tested commit/profile.

Do not broaden the task to work around a STOP condition.
