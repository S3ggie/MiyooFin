# Task 32 — Implement lazy indexed Home reads and bounded paging



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

Replace eager whole-library materialization with bounded indexed Home/Movies/Shows queries while preserving exact navigation/filter semantics.

## Depends On

- Task 31
- Task 30's application-comparator parity evidence.
- Existing schema-version/migration rules; any schema change remains limited
  to this task's derived-key representation and must be idempotent and
  recoverable.

## Allowed Files

- Home screen/model units
- `src/catalog/CatalogDbSchema.hpp` — the narrow schema-version migration and
  index for a MiyooFin-generated materialized organizational key, if required
  by the existing schema rules.
- `src/catalog/CatalogDb.hpp` and `src/catalog/CatalogDb.cpp` — only the
  worker-owned write/update maintenance and bounded keyset query APIs needed
  for Home paging, including scope/epoch handling.
- Home screen/model units and pure Home projection helpers — only bounded
  window state, continuation keys, cancellation, and UI publication wiring.
- `tests/test_main.cpp` and focused catalog/Home test case include(s).
- this task file and narrowly related SQLite migration documentation.

The canonical ordering design is settled for this task: MiyooFin generates a
deterministic materialized organizational key using the existing application
comparator semantics (ASCII-only case folding after removing a leading
ASCII-case-insensitive `The `), while retaining original title and ID as
separate tie-break columns. The key and indexes must be maintained atomically
whenever title data changes and used by deterministic bounded keyset queries.

An application-defined SQLite collation was considered but is not preferred:
it would make the index dependent on connection-local runtime registration and
collation implementation availability, complicate migration/recovery and
cross-build parity, and still would not provide an explicit continuation key.
A materialized key is durable, inspectable, migration-safe, and lets the
existing comparator remain the single source of truth without locale or
SQLite `NOCASE` behavior.

## Forbidden Scope

- No rendering redesign.
- No new database connection.
- No ImageCache/Download changes.
- No arbitrary page/cache/mmap PRAGMA tuning.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Measure current visible grid sizes and navigation expectations.
- Define page/window size constants from UI needs, not whole library size.
- Confirm organizational ordering parity from Task 30.

## Exact implementation requirements

1. Startup requests only tab/view metadata, Home rows, and first needed visible data.
2. Movies queries bounded page/window around visible grid with bounded prefetch.
3. Shows queries bounded page/window with current show/anime/alphabet behavior preserved.
4. Alphabet filter uses the reviewed materialized key and bounded keyset
   semantics only after exact parity is proven; never substitute SQLite
   `NOCASE` or locale-dependent Unicode folding.
5. Selection/scroll identity survives page refresh by item ID.
6. Prefetch is lower priority than current interactive page.
7. Cancel stale page requests on tab/filter/generation changes.
8. `m_movieMaster`, `m_showMaster`, etc. may only remain if bounded-window meaning is changed explicitly; do not retain full catalog under renamed fields.
9. DB queue pressure must not block frames.

## Invariants

- CatalogDb result must match the current ready scope epoch before publication; stale-scope results are ignored.
- Bounded query results/memory.
- Exact ordering/filter semantics.
- No UI DB.
- Local-first cached paging.

## Focused tests

- Libraries smaller/equal/larger than one page.
- Fast alphabet changes cancel stale results.
- Tab switch during pending query.
- Selection preservation.
- Offline show/movie filtering.
- No duplicates/gaps at page boundaries.
- Adversarial canonical ordering: mixed-case `The ` prefixes, ASCII-case
  equivalents, identical normalized keys and title/ID ties, arbitrary UTF-8,
  ASCII/UTF-8 mixtures, prefix relationships, empty/short titles, and equal
  keys split across page boundaries.
- Different page sizes concatenate to exactly the same global order.

## Complete validation commands

```sh
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
perf(home): load library metadata lazily from SQLite
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Exact ordering requires full-table materialization.
- Paging introduces duplicate/gap/selection bugs.
- UI thread waits for DB result.
- Needed schema change was not reviewed.

Do not broaden the task to work around a STOP condition.
