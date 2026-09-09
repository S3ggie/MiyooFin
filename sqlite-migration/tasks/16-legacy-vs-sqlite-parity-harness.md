# Task 16 — Legacy-vs-SQLite parity harness



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- After the successful task commit, **STOP for mandatory human review: CP-B — Migration/parity**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Build a repeatable host parity harness comparing old OfflineCatalog projections to SQLite results before any production read switch.

## Depends On

- Task 15

## Allowed Files

- `tests/**`
- Catalog test helpers
- OfflineLibraryProjection test-only use
- No production files unless a tiny read-only test seam is essential

## Forbidden Scope

- No runtime consumer switch and no App migration activation yet; Task 17 wires the already-proven importer into normal scope configuration.
- No dual-write.
- No legacy behavior change.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Collect representative synthetic fixtures and, if test-safe, anonymized structural shapes from existing catalog behavior.

## Exact implementation requirements

1. Compare full hierarchy IDs and order.
2. Compare every canonical MediaItem field.
3. Compare ordered genres and image tags.
4. Compare seasons(series) and episodes(season) results.
5. Compare offline projection behavior against identical DownloadSnapshot fixtures.
6. Compare downloaded-metadata fallback behavior when catalog hierarchy is missing.
7. Compare authoritative deletion outcomes.
8. Produce useful mismatch diagnostics in tests without secrets.

## Invariants

- Old implementation remains reference only.
- No runtime dual-authority.
- Parity failures block cutover.

## Focused tests

- Movies unsupported/population state explicit for phase A.
- Series with zero seasons.
- Multiple seasons/episodes.
- Duplicate legacy merge shapes.
- Download fallback.
- Unicode/text fields.
- Playback fields.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
test(catalog): prove legacy SQLite hierarchy parity
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Any unexplained parity mismatch.
- Test requires production behavior change.
- Legacy semantics reveal a schema-v1 incompatibility.

Do not broaden the task to work around a STOP condition.
