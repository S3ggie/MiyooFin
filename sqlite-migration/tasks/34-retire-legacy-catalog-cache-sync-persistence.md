# Task 34 — Retire legacy catalog/cache/sync persistence



## Execution mode — roadmap override

For this numbered SQLite roadmap task, the user's roadmap instruction overrides the repository `AGENTS.md` delegation preference **only for delegation/orchestration behavior**:

- The current/main Codex model performs the implementation directly using the currently selected **GPT-5.6 Luna High**.
- **Do not spawn implementation subagents.**
- **Do not spawn reviewer subagents by default.**
- Do not create SDD workspaces, generated implementation briefs, ledgers, handoff files, or orchestration artifacts.
- One numbered task equals **one narrow commit**. Do not combine adjacent tasks into one commit.
- The `Commit message` section in this task is explicit user authorization to commit **this task only** after every required validation succeeds.
- Preserve all other current repository `AGENTS.md` safety, dirty-work, threading, hardware-evidence, and validation rules.
- After the successful task commit, **STOP for mandatory human review: CP-H — Final retirement**. Do not begin the next numbered task until that checkpoint is explicitly approved.

## Goal

Only after every reconciliation/offline parity and hardware gate, remove obsolete OfflineCatalog, LibraryCache persistence, SyncStateStore persistence, and legacy runtime file writes while retaining deliberate rollback/compatibility policy.

## Depends On

- Task 33
- CP-G approval

## Allowed Files

- `src/cache/OfflineCatalog.*`
- `src/cache/LibraryCache.*`
- `src/cache/SyncState.*`
- Build/test source lists
- Migration compatibility code
- Docs/tests
- Call sites proven obsolete

## Forbidden Scope

- Do not delete ImageCache.
- Do not delete DownloadStore/DownloadManager persistence.
- Do not delete playback/session/telemetry stores.
- Do not delete user legacy files automatically without explicit lifecycle policy.

## Pre-change checks

```sh
git branch --show-current
git rev-parse HEAD
git status --short
```

- Code-search every legacy API call.
- Verify fresh-install, fresh-bootstrap, offline-with-downloads, and corruption-recovery tests exist.
- Confirm the human-approved rollback/compatibility window before removing retained legacy artifacts.

## Exact implementation requirements

1. Remove legacy production writers and obsolete whole-file serializers only after no runtime dependency remains.
2. Keep only the minimum explicitly approved rollback/compatibility handling; no `catalog.v1` importer is required by the SQLite bootstrap path.
3. Remove obsolete build entries/tests only when replaced.
4. Ensure startup no longer creates snapshot.v1/catalog.v1/sync-state.v1.
5. Do not automatically delete existing user legacy files unless a separately approved cleanup policy says when it is safe.
6. Update architecture/toolchain docs to final SQLite ownership.
7. Run complete host, ARM, migration, offline, playback, and hardware smoke validation.

## Invariants

- All separate subsystems remain separate.
- Upgrade path preserved.
- No silent user-data deletion.
- Final runtime uses indexed SQLite catalog.

## Focused tests

- Fresh install.
- Fresh bootstrap after upgrade.
- Already-SQLite install.
- Unsupported future schema.
- Corrupt DB rebuild.
- Offline with complete downloads.
- Network/local playback.
- No legacy writer call sites.

## Complete validation commands

```sh
make clean
make test -j2
make -j2
make onionos
make verify-arm
git diff --check
grep -R "OfflineCatalog::\|LibraryCache::save\|SyncStateStore::save" -n src || true
# final Miyoo smoke test is mandatory before CP-H approval
```

Do not report a command as passed unless it actually ran successfully. Hardware comments in the command block are required execution steps, not substitutes for evidence.

## Commit message

```text
refactor(catalog): retire legacy whole-file persistence
```

This task file is explicit user authorization to create exactly one commit for this task after all required validation succeeds. Commit no unrelated changes.

## STOP conditions

Stop and report if:

- Any supported upgrade still depends on removed legacy reader.
- Any runtime consumer still needs a legacy writer.
- Final Miyoo smoke test unavailable or fails.
- Retirement would delete unrelated cache/download/playback/session/telemetry data.

Do not broaden the task to work around a STOP condition.
