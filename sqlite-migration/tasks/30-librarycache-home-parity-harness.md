# Task 30 — LibraryCache/Home parity harness



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

Prove schema-v2 projections reproduce existing Home/library semantics before consumer cutover.

## Depends On

- Task 29

## Allowed Files

- `tests/**`
- Catalog query/projection helpers
- Small catalog DAL additions required strictly for parity

## Forbidden Scope

- No production Home switch.
- No lazy paging behavior change yet.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Enumerate `tabsFromSnapshot`, `offlineTabsFromSnapshot`, Movies/Shows organization, Continue Watching, Recently Added semantics.

## Exact implementation requirements

1. Compare legacy LibrarySnapshot and SQL reconstruction.
2. Compare view membership/order.
3. Compare Home rows/order.
4. Compare Movies/Shows item sets.
5. Compare offline Home filtering with identical DownloadSnapshot.
6. Prove existing organizational title behavior: leading `The ` ignored for organization, ASCII case-insensitive compare, deterministic ID tie-break.
7. Use EXPLAIN QUERY PLAN for future bounded view/Home queries.
8. Only if parity proves SQL needs an explicit derived organizational key/bucket, stop and add it through a reviewed schema migration rather than changing user-visible ordering.

## Invariants

- Exact user-visible semantics before optimization.
- No UI code change.
- No unproven SQL collation substitution.

## Focused tests

- `The` titles.
- Mixed ASCII case.
- Same organizational titles/tie-break.
- Anime/show grouping if applicable.
- Offline complete-download filtering.
- Home dynamic rows.

## Complete validation commands

```sh
make test -j2
make -j2
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
test(home): prove SQLite library projection parity
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Ordering mismatch.
- Offline projection mismatch.
- Fix would require changing current user-visible behavior.

Do not broaden the task to work around a STOP condition.
