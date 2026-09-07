# MiyooFin Codex Instructions

## Main Agent Role

The main Codex chat is a lightweight coordinator.

For any substantial coding, debugging, or implementation task:

1. Read the user's request.
2. Check the current Git HEAD and run `git status --short`.
3. Spawn one implementation subagent to perform the actual repository inspection, coding, and focused testing.
4. Give that subagent the user's full requirements and these repository rules.
5. Wait for the implementation subagent to finish.
6. Review its concise report and validation results.
7. Perform only minimal additional inspection if something failed, is unclear, or contradicts the requirements.
8. Give the user the final concise report.

The main agent should normally NOT:

* deeply inspect the codebase itself;
* spend substantial context reading implementation files;
* independently reproduce work already assigned to the implementation subagent;
* rewrite code already handled by the implementation subagent;
* repeatedly reread the repository just to verify routine successful work.

The implementation subagent is responsible for:

* locating the relevant code;
* understanding the affected architecture;
* making the narrowest correct changes;
* preserving unrelated work;
* adding or updating focused tests;
* running the required validation;
* returning a concise implementation report to the main agent.

Use only one code-editing subagent at a time.

For tiny and obvious edits, the main agent may implement directly.

Do not spawn scout or reviewer agents by default. Use an additional read-only subagent only when:

* the implementation is unusually risky;
* the coding subagent reports uncertainty;
* tests or builds fail;
* the result appears inconsistent;
* or the user explicitly requests another review.

The user should only need to interact with the main Codex chat.

## Git Safety

Before substantial work:

* Check the current HEAD.
* Run `git status --short`.
* Preserve all existing dirty work.
* Never reset, clean, discard, checkout over, or overwrite unrelated changes.
* Prefer narrow changes over broad speculative refactors.

Do not commit unless the user explicitly asks.

Do not deploy to the Miyoo Mini Plus unless the user explicitly asks.

Never commit runtime/generated files such as:

* `device.txt`
* `downloads/`

unless explicitly requested.

## MiyooFin Development Rules

MiyooFin targets the Miyoo Mini Plus running OnionOS.

Keep the SDL/UI thread responsive.

Never perform blocking work on the UI thread such as:

* HTTP requests
* curl transfers
* long filesystem operations
* retry sleeps
* large cache scans
* blocking worker joins that may wait on network operations

Use the existing bounded background-worker architecture.

Do not create detached threads.

Workers that reference screen-owned state must have safe cancellation and lifetime handling.

## Local-First Behavior

MiyooFin is local-first.

* Render cached content immediately when available.
* Preserve valid cached data through transient network failures.
* Offline browsing should continue wherever cached data is sufficient.
* Prefer a complete validated downloaded copy over remote streaming.
* Do not convert recoverable network failures into fatal browsing errors.

## Download Architecture

The current download architecture is hardware-verified and should not be replaced casually.

It uses:

* Jellyfin server-side transcoded HLS
* H.264 video
* AAC audio
* a constrained Miyoo-compatible profile
* segmented resumable downloads
* `.part` recovery
* the public/Cloudflare Jellyfin path
* per-segment retry handling
* local playback through the localhost bridge
* local-first playback resolution
* Movie downloads
* Episode downloads
* Season downloads
* Series downloads
* pause/resume/retry/delete
* OfflineCatalog
* offline playback journal
* download reconciliation

Preserve this architecture unless the requested task specifically requires changing it.

Do not introduce a LAN-only Jellyfin bypass unless explicitly requested.

Never log access tokens or full authenticated URLs.

## Debugging Workflow

When debugging:

1. Identify the concrete likely failure.
2. Gather evidence when needed.
3. Make the smallest justified fix.
4. Validate it.
5. Stop and report.

Do not bundle unrelated cleanup or features into a bug fix.

Do not claim hardware behavior is verified unless it was actually tested on the Miyoo Mini Plus.

## Validation

For normal C++ changes, run as applicable:

```sh
make test -j2
make -j2
git diff --check
```

For OnionOS/runtime changes, also run:

```sh
make onionos
make verify-arm
```

For playback bridge/reporter changes, also run as applicable:

```sh
make bridge bridge-test reporter reporter-test -j2
sh -n distributions/onionos/playback_runner.sh
```

Do not report a validation step as passed unless it actually ran successfully.

## Final Report

The main Codex chat—not the implementation subagent—reports the result to the user.

Keep the report concise and include:

* what changed;
* root cause when debugging;
* important files or systems affected;
* validation actually run;
* remaining uncertainty;
* whether anything was committed or deployed.

Do not dump large diffs, source listings, compiler output, or subagent transcripts unless specifically requested.

Do not commit or deploy merely because validation passed.

## Refactor Tasks

For numbered tasks under `refactor/tasks/`:

- Read `refactor/EXECUTION_RULES.md` and the current task file before editing.
- Execute exactly one numbered task at a time.
- Allowed Files are a hard boundary.
- One task equals one commit.
- For numbered refactor tasks, the task file's explicit commit instruction counts as user authorization to commit that task and overrides the general no-commit rule above.
- Run every validation command required by the task.
- Stop after the task commit; never start the next task automatically.

### Delegation

- GPT-5.6 Luna Low is the default implementation subagent for routine refactor work.
- Routine work includes pure-logic extraction, mechanical function moves, translation-unit splits, Makefile edits, rendering splits, test organization, and documentation.
- Use Luna Medium or High only for concrete complexity involving concurrency, worker lifecycle/ownership, networking/routing semantics, persistent formats, HLS behavior, playback handoff, or unexpected architectural coupling.
- State the concrete reason before escalating reasoning level.
- Do not use Terra Medium.
- Any subagent must obey the same repo rules, task scope, validation, commit, and STOP requirements.

### Orchestration

For routine numbered tasks:

1. Verify the worktree is clean.
2. Run the required pre-change validation.
3. Spawn at most one implementation subagent.
4. Do not create SDD workspaces, ledgers, generated briefs, review packages, or other orchestration artifacts.
5. Do not spawn a separate reviewer subagent.
6. The coordinating model reviews the resulting diff itself.
7. Run the task's focused validation and `make refactor-check`.
8. Do not run another `make test` after `make refactor-check`; it already runs the test suite.
9. Review `git diff --stat` and `git diff`.
10. Commit the single task, verify a clean worktree, then STOP.

A separate reviewer subagent or heavier workflow is reserved for genuinely high-risk tasks involving concurrency, networking, persistence, HLS, playback handoff, or concrete unexpected coupling.

Do not use heavy orchestration merely because a file is large or several files are touched.

### Direct execution for numbered refactor tasks

For numbered tasks under `refactor/tasks/`, the main Codex agent MUST perform the implementation directly.

This rule overrides the general Main Agent Role delegation instructions above for numbered refactor tasks.

- Do not spawn implementation subagents.
- Do not spawn reviewer subagents.
- Do not create SDD workspaces, ledgers, generated briefs, or review packages.
- Read the current task, inspect only the code necessary for that task, implement it directly, validate it, review the diff, commit it, and STOP.
- Use the currently selected Codex model and reasoning level for the entire task.
- If the task exposes unexpected coupling or cannot be completed safely within Allowed Files, STOP and report the problem instead of delegating or widening scope.
