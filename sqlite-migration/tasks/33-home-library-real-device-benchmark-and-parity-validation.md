# Task 33 — Home/library real-device benchmark and parity validation

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
- After the successful task commit, **STOP for mandatory human review: CP-G — Home/library cutover**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Prove final lazy Home/library SQLite architecture on real Miyoo hardware before legacy retirement.

## Depends On

- Task 32

## Allowed Files

- `docs/sqlite-migration-benchmark.md` — append factual real-device evidence for Task 33.
- No application/source changes in this benchmark task; any required fix is a STOP condition and must be handled by a separately reviewed roadmap amendment/task.

## Forbidden Scope

- No legacy retirement yet.
- No hardware completion claim without actual Miyoo.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Build clean ARM package.
- Preserve comparable pre-Home-migration telemetry.
- Record catalog/library counts/environment.

## Exact implementation requirements

1. Run cold/warm startup measurements.
2. Measure time to usable cached Home.
3. Measure Movies first display and Shows first display.
4. Stress alphabet filters/tab switching.
5. Measure CPU, RSS/peak, process reads/writes, DB query/queue latency, UI stalls.
6. Verify offline Home, Series, EpisodeBrowser, downloads, local/network playback.
7. Verify hierarchy sync still meets CP-F behavior.
8. Compare against both pre-SQLite and hierarchy-only baselines where metrics exist.
9. Record any regression explicitly.

## Invariants

- Real hardware.
- Full functional parity.
- No legacy deletion.

## Focused tests

- Cold/warm startup.
- Large Movies/Shows navigation.
- Offline mode.
- Download/local playback.
- Hierarchy refresh during navigation.

## Complete validation commands

```sh
make onionos
make verify-arm
# deploy through normal OnionOS package/launcher
# execute BENCHMARK_PROTOCOL Home/library scenarios
# collect/decode telemetry
# execute offline/download/local+network playback checks
git status --short
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
perf(home): validate lazy SQLite library on Miyoo
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- No physical Miyoo evidence.
- Startup/Home materially regress without accepted tradeoff.
- Offline or playback behavior regresses.
- DB queue latency causes UI stalls.

Do not broaden the task to work around a STOP condition.
