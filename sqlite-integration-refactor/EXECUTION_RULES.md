# Execution Rules — SQLite Integration Refactor

These rules apply to every numbered file under `sqlite-integration-refactor/tasks/`.

## Model and coordination

The main coordinator must use **GPT-5.6 Luna Low**.

Any subagent used for this roadmap must also use **GPT-5.6 Luna Low**. Do not escalate to another model unless the user explicitly changes this roadmap rule.

Maximum concurrency is **2 subagents**. Repository `AGENTS.md` still limits implementation to one code-editing subagent at a time, so a second concurrent subagent may be read-only validation/review work only.

For an ordinary implementation task, the coordinator may use one Luna Low implementation subagent, review its concise result, run/confirm required validation, and commit the one task. Do not create agent swarms, SDD ledgers, broad implementation briefs, or orchestration artifacts.

## Task startup

Before editing any task:

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

Then read, in this order:

1. repository `AGENTS.md`;
2. this `EXECUTION_RULES.md`;
3. the numbered task file;
4. the directly relevant source/tests named by **Allowed Files**.

Do not begin by scanning or refactoring unrelated subsystems.

## Dirty-work safety

Preserve all existing dirty work.

Never run `git reset`, `git clean`, discard, restore, checkout-over, or otherwise overwrite user changes. Do not stash user work merely to make a task easier.

If dirty work overlaps an Allowed File, inspect it and preserve it. If the task cannot be completed safely without overwriting or untangling user changes, STOP and report the conflict.

A task does not require a clean worktree unless its own file explicitly says so.

## Scope discipline

**Allowed Files is a hard boundary.**

Do not edit outside Allowed Files because another cleanup looks convenient. If correctness genuinely requires another production path, STOP and report the needed path and reason instead of silently broadening scope.

Do not combine adjacent numbered tasks. Do not pre-implement a later phase.

## Architecture invariants

Every task preserves all of the following unless the user explicitly approves a new architecture based on evidence:

- schema v3 baseline;
- one CatalogDb SQLite worker;
- one SQLite connection for the active scope;
- bounded priority/work queues;
- indexed/keyset bounded library reads;
- no full-library RAM materialization;
- atomic fresh-database temporary-file bootstrap/promotion;
- no SQLite on the SDL/UI thread;
- no blocking HTTP/large filesystem/retry sleep on the SDL/UI thread;
- DownloadStore as physical offline-availability authority;
- ImageCache as artwork-byte cache;
- no new production dual-write to legacy whole-file persistence.

The FFplay/mmiyoo audio-only display-handoff bug is explicitly outside this roadmap.

## Ownership target

Implementation decisions must move toward, not away from, these ownership rules:

1. App/session lifecycle configures/deconfigures CatalogDb scope.
2. LibrarySync owns Jellyfin -> SQLite synchronization.
3. CatalogDb owns SQLite execution only.
4. LibraryQuery owns domain-level catalog reads.
5. Home/Series/Episode screens consume results; they do not own DB synchronization.
6. OfflineLibraryQuery combines DownloadStore availability with catalog metadata/fallback.
7. DownloadManager owns download-specific planning/transfer behavior, not generic hierarchy synchronization.

If a task appears to require violating these rules, STOP rather than inventing an exception.

## Implementation method for Luna Low

Keep each task mechanically small:

1. identify the exact current code path named by the task;
2. write or update the focused regression test first when practical;
3. make the minimum production change needed for that task;
4. run the focused validation command;
5. run every complete validation command in the task;
6. inspect `git diff --check` and the task diff;
7. create exactly the task's one commit only after validation passes.

Prefer explicit structs and concrete small components over templates, callback frameworks, generic repositories, service locators, or dependency-injection systems.

Reuse current cancellation/future/thread patterns unless the task specifically replaces ownership.

## Validation truthfulness

Do not claim success for a command that was not run successfully.

If `make onionos` or `make verify-arm` is required, a host build is not a substitute.

If a task says physical Miyoo evidence is mandatory, ARM compilation is not hardware evidence.

A validation failure is not permission to weaken a test or perform unrelated cleanup.

## Commit policy

Each numbered task file explicitly authorizes **one commit for that task only** after all required validation passes.

Use the exact commit message in that task's **Commit boundary** section unless a technical Git limitation requires a trivially equivalent wording.

Never include the next task in the same commit.

Do not amend/rewrite previous user commits just to keep history tidy.

## Autonomous flow and checkpoints

Do not ask for user confirmation between ordinary tasks within an approved phase.

Mandatory checkpoints are:

- **CP-A after Task 09** — correctness foundation;
- **CP-B after Task 17** — ownership/modularity;
- **CP-C after Task 31** — physical-device correctness;
- **CP-D after Task 33** — performance rebaseline.

After committing the checkpoint task, STOP and wait for explicit approval before starting the next phase.

Task-specific STOP conditions also stop execution immediately.

## Hardware tasks

Only tasks that explicitly say **MANDATORY** in their Hardware gate require device runtime evidence.

Hardware-evidence tasks are evidence-only unless their Allowed Files say otherwise. If the device exposes a production defect, STOP and create/review a separate narrow corrective task; do not patch application code inside an evidence task.

Use the established OnionOS package/remote-launch/SSH workflow. Do not improvise a new deployment system inside this roadmap.

## Original SQLite Task 34 is paused

`sqlite-migration/tasks/34-retire-legacy-catalog-cache-sync-persistence.md` remains **PAUSED for this entire roadmap**.

Do not:

- execute it;
- partially retire its legacy files early;
- delete user legacy artifacts;
- remove compatibility readers merely because a new task makes them unused;
- interpret CP-D as automatic authorization.

After Task 33, report the evidence and wait for a separate explicit user decision about legacy retirement.
