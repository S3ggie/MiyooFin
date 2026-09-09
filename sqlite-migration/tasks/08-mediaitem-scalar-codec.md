# Task 08 — MediaItem scalar codec



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

Map all scalar MediaItem semantics to/from `media_items` rows with exact defaults and type conversion.

## Depends On

- Task 07

## Allowed Files

- New `src/catalog/MediaItemSql.*`
- `src/catalog/CatalogDbSql.*` as needed
- Focused tests
- Build files

## Forbidden Scope

- No genre/image-tag persistence yet.
- No hierarchy DAL.
- No screen integration.
- Do not change `MediaItem` semantics.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate every scalar field in current `MediaItem.hpp`.
- Enumerate current Jellyfin normalized type strings.

## Exact implementation requirements

1. Implement explicit kind conversion for movie/show/season/episode.
2. Bind/read every scalar: id, title, overview, year, rating, etag, played, progress, playback ticks, indexes, runtime, series name/id, season id, art RGB.
3. Use explicit SQLite bind/column APIs; no raw struct serialization.
4. Preserve empty-string/zero semantics.
5. Reject missing primary ID for durable rows.
6. Do not invent data normalization not already present.

## Invariants

- Round-trip does not change MediaItem scalar meaning.
- No whole-catalog materialization.
- No schema change.

## Focused tests

- Full populated item per kind.
- All-default item with ID/kind.
- Boundary RGB/index/tick values.
- Unknown/invalid kind rejected.
- Nullable relationships round-trip.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
feat(catalog): map MediaItem scalars to SQLite rows
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- A current MediaItem scalar cannot be represented losslessly.
- A schema change is required—stop and review before altering v1.

Do not broaden the task to work around a STOP condition.
