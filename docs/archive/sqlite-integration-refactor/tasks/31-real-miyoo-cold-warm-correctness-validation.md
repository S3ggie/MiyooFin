# Task 31 — Validate cold/warm correctness on real Miyoo

**Phase:** C — validation

## Objective

Validate the refactored synchronization/query/offline architecture on a physical Miyoo Mini Plus before performance work.

## Why

Host tests cannot prove OnionOS filesystem timing, actual route behavior, memory pressure, or real lifecycle interactions.

## Preconditions

- Tasks 18-30 pass.
- A physical Miyoo Mini Plus is available through the established OnionOS launcher/SSH workflow.

## Allowed Files

- Create or update `docs/sqlite-integration-refactor-validation.md` with factual evidence only
- No production source files

## Forbidden Scope

- Do not patch production behavior inside this validation task. If a scenario fails and needs code, STOP and create/review a focused corrective task before rerunning this validation task.
- Do not weaken acceptance criteria to match incorrect device behavior.
- Do not perform playback repair.

## Architecture invariants

- Schema v3 remains the baseline. Do not introduce schema v4 or redesign the schema unless this task explicitly requires it; no task in this roadmap currently does.
- Exactly one `CatalogDb` SQLite worker owns exactly one SQLite connection for the active scope.
- No SQLite operation, HTTP request, long filesystem operation, retry sleep, or blocking worker join may run on the SDL/UI thread.
- Keep indexed/keyset bounded reads and bounded result windows; do not reintroduce whole-library RAM materialization.
- Keep the atomic fresh-database temporary-file bootstrap/promotion path unchanged unless a task explicitly targets it.
- `DownloadStore` remains authoritative for physical offline availability. Catalog metadata may enrich downloads but must not decide whether bytes exist.
- `ImageCache` remains the artwork-byte cache and is not replaced by SQLite.
- Do not add production dual-write to legacy whole-file catalog/cache persistence.
- The intermittent audio-only FFplay/mmiyoo display bug is out of scope. Do not modify playback repair code in this roadmap.

## Implementation requirements

1. Build a clean ARM package from the exact Task 30 commit.
2. Run a fresh-catalog launch and verify supported Movies/Shows/Anime populate correctly.
3. Run a warm-catalog relaunch and verify committed library content is usable before full refresh completes.
4. Exercise multiple TV libraries and Anime navigation.
5. Exercise manual/offline startup with the legacy LibraryCache artifact absent while preserving downloads; verify downloaded roots.
6. Trigger graceful exit while synchronization is active and verify MiyooFin exits and MainUI restores.
7. Record commit, environment, observed counts/timestamps, pass/fail, and limitations in the validation doc.
8. Do not turn this into a playback-bug investigation.

## Tests

- Fresh database PASS.
- Warm database first-use PASS.
- Multiple-TV/Anime PASS.
- Offline without LibraryCache PASS.
- Exit-during-sync/MainUI restore PASS.

## Validation

Run the following from the repository root. Do not report a command as passed unless it actually completed successfully.

```sh
make test -j2
make -j2
make onionos
make verify-arm
# Deploy through the established OnionOS package/remote-launch workflow.
# Execute the five hardware scenarios above and collect factual logs/evidence.
git status --short
git diff --check
```

## Hardware gate

MANDATORY. This task cannot complete without physical Miyoo Mini Plus evidence. ARM compilation alone is not hardware evidence.

## STOP conditions

- The change needs a production file outside **Allowed Files**.
- The task appears to require schema v4, a second SQLite connection/worker, full-library materialization, or SQLite/network work on the SDL thread.
- A prerequisite task/checkpoint is missing or the current repository state contradicts the task assumptions.
- Required validation fails for a reason outside this task's narrow scope.
- Physical Miyoo evidence is unavailable.
- Any required correctness scenario fails or shows stale/incorrect membership.
- Warm Home still waits for full network population instead of committed SQLite.
- Offline downloaded roots require legacy LibraryCache.
- Exit during sync fails to restore MainUI.

## Commit boundary

This task is exactly one commit. Commit only files allowed above after all required validation passes.

```text
test(hardware): validate sqlite refactor correctness
```

Do not include any part of Task 32 in this commit.

## Checkpoint

**CP-C — Hardware correctness. STOP after committing factual evidence and obtain explicit approval before Phase D.**

Do not start the next phase until this checkpoint is explicitly approved.
