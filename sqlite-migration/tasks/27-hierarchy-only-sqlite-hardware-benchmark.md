# Task 27 — Hierarchy-only SQLite hardware benchmark

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
- After the successful task commit, **STOP for mandatory human review: CP-F — Hierarchy-only hardware benchmark**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Prove the hierarchy-only SQLite architecture beats or justifiably replaces the current whole-file OfflineCatalog on real Miyoo hardware before any LibraryCache/Home migration.

## Depends On

- Task 26
- CP-E approval

## Allowed Files

- `docs/sqlite-migration-benchmark.md` — append factual real-device evidence for Task 27.
- No application/source changes in this benchmark task; any required fix is a STOP condition and must be handled by a separately reviewed roadmap amendment/task.

## Forbidden Scope

- No LibraryCache/Home migration.
- No claiming success without Miyoo evidence.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Preserve/identify pre-SQLite optimization baseline telemetry.
- Build tested commit cleanly.
- Record DB/library/environment.

## Exact implementation requirements

1. Run benchmark protocol hierarchy-only A/B scenarios.
2. Measure library/hierarchy sync duration, process writes/reads, CPU, RSS/peak, Series cached load, EpisodeBrowser cached load, worker completion/failure/cancellation, UI stalls, startup, download planning.
3. Verify offline browsing and local playback.
4. Verify checkpoint after failure/cancellation with real runtime behavior.
5. Compare against approved current optimization baseline where data is comparable; explicitly label missing baseline metrics rather than inventing them.
6. Document whether expected write-amplification reduction is observed.

## Invariants

- Real Miyoo evidence.
- Functional parity required even if performance improves.

## Focused tests

- Cold/warm start.
- Changed hierarchy.
- Large hierarchy.
- Series/Episode navigation.
- Offline/download playback.
- Failure/cancellation checkpoint.

## Complete validation commands

```sh
make onionos
make verify-arm
# deploy normal package to Miyoo
# execute BENCHMARK_PROTOCOL hierarchy-only scenarios
# collect/decode MFT telemetry and CatalogDb metrics
# compare to preserved baseline
# run offline/local playback functional checks
git status --short
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
perf(catalog): validate hierarchy SQLite architecture on Miyoo
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- No real hardware.
- Major functional parity regression.
- Writes/RSS/UI stalls materially regress without accepted explanation.
- Checkpoint invariant fails.

Do not broaden the task to work around a STOP condition.
