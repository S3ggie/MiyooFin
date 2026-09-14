# Refactor2 Autonomous Execution Rules

These rules apply to every numbered file in `Refactor2/tasks/`.

## Autonomous command

When instructed to “execute Refactor2,” read this file and `Refactor2/README.md`, then execute every incomplete numbered task in ascending order. Do not ask for confirmation between successful tasks. Each task is one reviewable commit; the roadmap is explicit authorization to create those task commits only. Never push, deploy, rewrite history, amend pre-existing commits, or commit unrelated work.

## Startup and resumption

Before every task:

1. Read the current root `AGENTS.md`, this file, the task file, and the preceding task's commit/diff when one exists.
2. Run `git rev-parse --short HEAD` and `git status --short`.
3. If resuming, inspect `git log --oneline -20` and identify the first task whose exact commit subject is absent. Never repeat a completed task.
4. Preserve every pre-existing dirty file. If a dirty file overlaps the task's allowed files and its changes were not produced by the current task, STOP and report the collision.
5. Run the task's preflight focused test before editing. A pre-existing failure is a STOP condition.

## Non-negotiable invariants

- This is behavior-preserving modularization. Do not change UI behavior, strings, navigation, sorting, pagination, SQL, schema version, migrations, JSON formats, endpoint paths, timeouts, retry policy, telemetry schema, persistence formats, or public API signatures unless a task explicitly says so.
- Keep SDL/UI work nonblocking. Do not move HTTP, curl, SQLite waits, cache scans, filesystem scans, sleeps, or blocking joins onto the UI thread.
- Do not create detached threads. Preserve cancellation, wakeup, future, mutex, and lifetime ordering exactly.
- Preserve local-first rendering, valid cached state on transient failures, offline browsing, and complete-download-first playback.
- Preserve the current Jellyfin transcoded HLS download architecture, profiles, segment retries, `.part` recovery, public/Cloudflare path, localhost bridge, reconciliation, and pause/resume/retry/delete semantics.
- `CatalogDb` keeps its one-worker/one-active-connection execution model. Do not introduce another connection, ORM, repository framework, schema change, or whole-library materialization.
- `LibrarySync` remains the Jellyfin-to-SQLite synchronization owner. Screens remain consumers/coordinators.
- `DownloadStore` remains authoritative for physical offline availability. `ImageCache` remains authoritative for artwork bytes.
- Do not add dependencies, generic frameworks, service locators, or speculative abstractions. Prefer moving existing method definitions into concern-specific translation units and adding only small private helpers needed to remove real duplication.
- Do not edit or commit `device.txt`, `downloads/`, `output/`, logs, benchmark captures, credentials, access tokens, or authenticated URLs.
- Do not deploy to the Miyoo Mini Plus. Never claim hardware verification.

## Mechanical implementation method

1. Inspect the exact symbols and tests named by the task; do not scan unrelated subsystems.
2. Add or reorganize focused tests first when behavior coverage is missing. A pure test-file split must not alter assertions or test order within a case.
3. Make the smallest extraction. Prefer moving definitions byte-for-byte and retaining the owning class/interface.
4. Add new `.cpp` files to every applicable host, cross, desktop, and test production-source list. Do not use source globs.
5. Run the focused test target immediately.
6. Run all task validation commands exactly as written.
7. Inspect `git diff --check`, `git status --short`, and `git diff --stat`. Inspect the actual diff and remove accidental formatting churn.
8. Stage only allowed files and commit with the task's exact subject.
9. Confirm the worktree contains no uncommitted task changes, then continue.

## Universal STOP conditions

STOP without committing and report the exact evidence if:

- the task requires a production file outside its Allowed Files;
- a prerequisite commit is missing or current code contradicts the task's assumptions;
- preserving behavior requires changing a public signature, data format, SQL/schema, endpoint, worker ownership, or architecture invariant not explicitly authorized;
- a required validation fails and the failure cannot be corrected within Allowed Files;
- a test appears flaky: rerun the exact focused command once; if results differ, STOP;
- the change grows beyond roughly 500 net new non-test lines or requires a new subsystem rather than an extraction;
- the task exposes a pre-existing correctness bug. Record it; do not fold the fix into the refactor;
- hardware evidence would be required to decide correctness.

Compilation errors directly caused by a moved definition may be fixed within Allowed Files. Do not broaden scope to clean unrelated warnings.

## Commit and final report policy

Each task is exactly one commit with the exact subject in its task file. A failed task gets no commit. After Task 16, report commits created, files/modules introduced, validation actually run, remaining uncertainty, and explicitly state that nothing was deployed. Do not push.
