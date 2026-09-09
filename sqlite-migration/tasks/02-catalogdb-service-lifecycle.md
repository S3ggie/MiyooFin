# Task 02 — CatalogDb service lifecycle



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

Introduce the app-scoped CatalogDb service shell with one owned/joined worker and no database behavior yet.

## Depends On

- Task 01

## Allowed Files

- New `src/catalog/CatalogDb.hpp`
- New `src/catalog/CatalogDb.cpp`
- `src/app/App.hpp`
- `src/app/App.cpp` only for service lifetime ownership
- `Makefile`
- `Makefile.cross`
- `tests/test_main.cpp` / focused test include files

## Forbidden Scope

- No schema.
- No media reads/writes.
- No screen integration.
- No DownloadManager changes.
- No SQLite connection opening yet if Task 05 has not been reached.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Trace `App` construction/destruction order.
- Confirm current worker join conventions and `deferDestruction` behavior.
- Confirm service can be destroyed before SDL/curl teardown without blocking the UI loop.

## Exact implementation requirements

1. Create one app-scoped service object with deterministic construction/destruction.
2. Create exactly one worker thread owned by the service.
3. Worker waits on a condition variable and exits through an explicit stop flag.
4. Destructor requests stop, wakes, and joins the worker.
5. No `detach()`.
6. Do not expose raw worker primitives or future SQLite handles to screens.
7. Service may initially process a test-only no-op job to prove lifecycle.

## Invariants

- One worker.
- Owned and joined.
- No SDL/UI blocking during normal use.
- No SQLite calls yet.
- No detached threads.

## Focused tests

- Construct/destroy repeatedly.
- Idle worker shutdown.
- Shutdown with queued no-op work.
- No job runs after destruction begins.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
grep -R "\.detach()" -n src/catalog src/app || true
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): add owned CatalogDb service lifecycle
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Lifecycle requires detached work.
- Destructor can race screen-owned state.
- App ownership would require unrelated startup refactor.

Do not broaden the task to work around a STOP condition.
