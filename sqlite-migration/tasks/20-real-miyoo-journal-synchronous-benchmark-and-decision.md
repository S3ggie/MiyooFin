# Task 20 — Real Miyoo journal/synchronous benchmark and decision

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
- After the successful task commit, **STOP for mandatory human review: CP-D — Journal benchmark**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Run the complete journal matrix on Miyoo hardware, perform required recovery testing, and record the evidence-backed candidate production profile for CP-D approval.

## Depends On

- Task 19

## Allowed Files

- `docs/sqlite-migration-benchmark.md` — append exact journal-matrix/recovery evidence.
- `src/catalog/` — update the existing CatalogDb runtime-profile/config implementation only if the hardware winner differs from DELETE+FULL, plus focused profile tests; no other application files.

## Forbidden Scope

- No tuning page_size/cache/temp/mmap.
- No second connection.
- No WAL selection before measured evidence.
- No claim from host-only results.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Record environment and baseline per protocol.
- Use isolated benchmark DB/path.
- Confirm user/human approval before any real hard-power interruption.

## Exact implementation requirements

1. Run A–F with minimum repetition from BENCHMARK_PROTOCOL.
2. Measure transaction/generation time, process I/O, RSS, file sizes, error counts.
3. Run mandatory process-interruption recovery for every candidate.
4. Run controlled power-interruption test for finalists only when explicitly approved; otherwise record NOT PERFORMED and CP-D must decide whether evidence is sufficient.
5. Run app-level hierarchy workload on at least baseline and leading finalist(s).
6. Apply the evidence-backed winner in this task using the decision criteria in `BENCHMARK_PROTOCOL.md`. If the matrix is ambiguous, retain DELETE+FULL rather than guessing. The mandatory CP-D review occurs **after this task's single commit** and may accept or reject that evidence-backed choice.
7. If the measured winner differs from DELETE+FULL, change only the CatalogDb profile default/tests plus benchmark evidence in this task. If evidence is insufficient, keep DELETE+FULL and record that conservative decision; CP-D reviews it after the commit.
8. Record why each rejected profile lost.

## Invariants

- Hardware evidence required.
- No speculative tuning.
- Checkpoint semantics evaluated.

## Focused tests

- DB quick/FK check after interruptions.
- Profile persists/opens correctly.
- No unexpected -shm requirement for EXCLUSIVE candidates if SQLite documentation predicts none.

## Complete validation commands

```sh
make onionos
make verify-arm
# execute ARM benchmark matrix A-F on /mnt/SDCARD isolated benchmark directory
# decode/compare telemetry with existing analysis tooling
# execute process-interruption recovery tests
# execute explicitly approved hard-power finalist tests, or record NOT PERFORMED
# rerun selected profile and DELETE+FULL control
git status --short
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
perf(sqlite): select Miyoo-validated journal profile
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- No real Miyoo evidence.
- Any candidate corrupts DB in required recovery tests.
- Hard-power testing is required by reviewer but not authorized.

Do not broaden the task to work around a STOP condition.
