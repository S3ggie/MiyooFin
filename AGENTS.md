# MiyooFin Codex Instructions

## Main Agent Role

The main Codex chat is the default implementer for normal coding, debugging, refactoring, and testing.

For substantial coding, debugging, refactoring, or testing tasks, the main agent should:

1. Read the user's request.
2. Check the current Git HEAD and run `git status --short`.
3. Inspect the relevant repository areas, make the narrowest correct changes, and preserve unrelated work.
4. Add or update focused tests when appropriate.
5. Run the required validation.
6. Give the user the final concise report.

Use subagents selectively only when:

* multiple investigations are genuinely independent;
* a risky change benefits from independent review;
* the main agent is blocked or materially uncertain;
* or the user explicitly requests one.

When a subagent is used, give it the user's full requirements and these repository rules. The subagent may be responsible for:

* locating the relevant code;
* understanding the affected architecture;
* making the narrowest correct changes;
* preserving unrelated work;
* adding or updating focused tests;
* running the required validation;
* returning a concise report to the main agent.

Use only one code-editing subagent at a time.

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

## Test Organization

* Add tests to the smallest relevant focused test group.
* Do not recreate `tests/test_main.cpp`.
* Give each new major subsystem its own `tests/test_<group>.cpp` binary.
* Register new test binaries in `TEST_GROUPS` and the shared list in `tests/test_runner.sh`.
* Keep shared fixtures in `tests/test_support.hpp` or a narrowly scoped support file.
* Do not include production `.cpp` files from tests.
* If a test group exceeds roughly 1,000 lines or takes more than 15–20 seconds to compile, split it instead of extending it indefinitely.
* During development, run the focused binary first, then run `make test -j2` once before completion.
* Do not use `make -B` unless dependency tracking is proven incorrect.

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

## Repository Discovery

* After the repository is indexed with Context Mode, prefer `ctx_search` for broad repository discovery and questions about where functionality, architecture, or related code lives.
* Prefer `ctx_batch_execute` when several repository searches or inspections can be gathered together.
* Use normal `Search`, `Read`, `rg`, or other native tools for narrow exact verification once the relevant files or symbols are known.
* Do not use Context Mode mechanically when a direct targeted read or exact search is simpler.
* Avoid repeatedly searching or rereading repository areas that Context Mode has already located unless verification is necessary.
