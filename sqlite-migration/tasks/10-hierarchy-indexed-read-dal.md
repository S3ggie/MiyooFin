# Task 10 — Hierarchy indexed read DAL



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

Add worker-side indexed queries for seasons by series and episodes by season.

## Depends On

- Task 09

## Allowed Files

- `src/catalog/CatalogDb.*`
- Catalog SQL/codec files
- Focused tests

## Forbidden Scope

- No writes beyond fixture setup.
- No screen integration.
- No OfflineLibraryProjection rewrite yet.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Use EXPLAIN QUERY PLAN in host tests to verify intended indexes are usable.
- Confirm exact existing sort behavior in Series/EpisodeBrowser.

## Exact implementation requirements

1. Implement async CatalogDb jobs `GetSeasons(seriesId)` and `GetEpisodes(seasonId)`.
2. Use prepared cached SELECT statements.
3. Return bounded vectors containing only the requested relationship.
4. Order seasons/episodes by index_number then existing compatible tie-break semantics.
5. Check cancellation/generation before query and before publishing result.
6. Do not expose sqlite rows/handles outside worker.

## Invariants

- No full catalog read.
- Interactive read priority.
- UI receives copied MediaItem vectors only.

## Focused tests

- Correct relationship filtering/order.
- Empty relationship.
- Large fixture still returns only requested branch.
- Index query-plan test.
- Cancellation/result suppression.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): add indexed hierarchy read queries
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Query plan scans the entire media_items table for relationship reads.
- Existing ordering cannot be reproduced without schema review.

Do not broaden the task to work around a STOP condition.
