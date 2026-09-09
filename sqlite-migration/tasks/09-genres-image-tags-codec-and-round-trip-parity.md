# Task 09 — Genres/image-tags codec and round-trip parity



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

Complete MediaItem row parity by persisting ordered genres and arbitrary image tags without duplication.

## Depends On

- Task 08

## Allowed Files

- `src/catalog/MediaItemSql.*`
- Catalog statement registry files
- Focused tests

## Forbidden Scope

- No hierarchy consumer integration.
- No ImageCache changes.
- No JSON blob columns.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Confirm current `genre` is derived from the first `genres` entry.
- Collect test fixtures with zero, one, and multiple genres/tags.

## Exact implementation requirements

1. Replace genre child rows atomically for an updated item.
2. Replace image-tag child rows atomically for an updated item.
3. Preserve genre ordinal/order.
4. Reconstruct `MediaItem::genre` from first ordered genre.
5. Preserve arbitrary image-type/tag pairs.
6. Ensure item delete cascades child rows.
7. Add a canonical MediaItem equality helper for catalog parity tests if existing `LibraryCache::itemEquivalent` is insufficient due etag/art fields.

## Invariants

- No ImageCache ownership change.
- No duplicate `genre` storage.
- Child rows never outlive item.

## Focused tests

- Zero/multi genres.
- Zero/multi tags.
- Update removing child rows.
- Delete cascade.
- Canonical full MediaItem round-trip.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
test(catalog): prove complete MediaItem SQLite parity
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Legacy/current MediaItem semantics conflict with schema.
- Parity requires changing ImageCache or Jellyfin parser.

Do not broaden the task to work around a STOP condition.
