# MiyooFin SQLite Migration Execution Roadmap — v2

Target repository: `S3ggie/MiyooFin`  
Approved optimization base when the roadmap was designed: `main` after `b9efee216ad40e17b0c787c959a3a91fd8748d5a`  
Executor target: the currently selected **GPT-5.6 Luna High** in the main/current Codex model.

This ZIP is an execution roadmap only. It contains no application implementation.

## Final task count

**34 numbered tasks.**

Two first-class tasks were added after the second audit:

- Task 04 — CatalogDb scope/session lifecycle.
- Task 17 — normal application migration activation on valid scope configuration.

All later task numbers/dependencies were shifted and re-audited.

## Direct autonomous execution rule

For every file under `tasks/`:

- the main/current Codex model implements directly;
- no implementation subagents;
- no reviewer subagents by default;
- no SDD workspaces, briefs, ledgers, or orchestration artifacts;
- one task = one commit;
- that task file's Commit message is explicit authorization for exactly that commit after successful validation;
- in autonomous roadmap mode, continue directly to the next ordinary task after committing;
- STOP at mandatory human checkpoints or genuine task STOP conditions.

All non-delegation safety/validation rules in the repository's current `AGENTS.md` still apply.

## Approved architecture retained

- official pinned SQLite 3.53.4 amalgamation;
- same vendored `sqlite3.c` for host and ARM;
- SQLite compiled separately with `-Os`;
- one app-scoped CatalogDb service;
- one owned/joined worker;
- one SQLite connection initially;
- server/user-scoped database using existing LibraryCache scope derivation;
- no SQLite calls on SDL/UI thread;
- bounded priority job queue;
- prepared statement reuse;
- OfflineCatalog migration first;
- Home/LibraryCache migration only after hierarchy-only real-device approval;
- ImageCache, DownloadStore/DownloadManager, playback/session files, and telemetry files remain separate;
- legacy catalog untouched during migration;
- hierarchy checkpoint advances only after a complete successful generation;
- no production dual-write;
- journal/synchronous profile decided by real Miyoo Mini Plus hardware evidence.

## Scope lifecycle added by v2

CatalogDb exists for the app lifetime but starts unconfigured.

```text
valid Session(server,user)
        ↓
configureScope(...)
        ↓ (caller immediately advances requested scope epoch)
old results become unpublishable
        ↓
CatalogDb worker scope-control job
        ↓
cancel/reject stale queued epoch work
finalize old statements
close old scoped DB
open/migrate latest scoped DB
        ↓
Ready(epoch)
```

Logout uses `deconfigureScope()` and immediately invalidates the previous epoch.

Changing server/account can never expose rows/results from the prior scope.

## Migration activation made explicit

After migration/parity code is proven, Task 17 wires normal App lifecycle to CatalogDb:

- valid saved session -> nonblocking `configureScope`;
- successful login -> nonblocking `configureScope`;
- logout/account/server change -> `deconfigureScope`.

On the worker, a valid latest scope:

1. opens a supported existing SQLite DB; or
2. if final DB is absent and legacy `catalog.v1` exists, runs the validated `.migrating` import/promotion; or
3. creates a fresh DB.

Before consumer cutover, this SQLite DB is validated/shadow storage only. Existing legacy reads stay authoritative until their numbered switch tasks.

Thus Task 18 can trigger real migration through a **normal Onion launch/login**, not a hidden diagnostic path.

## MFT v1 frozen

`telemetry/SCHEMA_V1.md` is immutable for this migration.

CatalogDb needs new worker/summary semantics, so Task 13 deliberately creates **MFT v2** with:

- a separate `telemetry/SCHEMA_V2.md`;
- explicit schema-version bump;
- unchanged v1 record layouts/meanings;
- v2 CatalogDb worker identity;
- v2-only fixed CatalogDb summary;
- decoder/analyzer support for both v1 and v2;
- golden tests proving existing v1 traces still decode.

See `TELEMETRY_V2_PLAN.md`.

## Mandatory checkpoints

1. **CP-A** after Task 07 — SQLite build/core/scope/schema.
2. **CP-B** after Task 16 — migration/parity.
3. **CP-C** after Task 18 — real-device migration.
4. **CP-D** after Task 20 — journal/synchronous benchmark and profile choice.
5. **CP-E** after Task 26 — hierarchy consumer cutover.
6. **CP-F** after Task 27 — hierarchy-only hardware benchmark.
7. **CP-G** after Task 33 — Home/library cutover.
8. **CP-H** after Task 34 — final retirement.

## Task map

| Approved stage | v2 tasks |
|---|---|
| Vendor/build SQLite | 01 |
| CatalogDb core/queue/scope/connection | 02–05 |
| Schema | 06–07 |
| MediaItem codec/parity | 08–09 |
| Hierarchy DAL | 10–12 |
| SQLite telemetry / MFT v2 | 13 |
| Legacy migration machinery | 14–15 |
| Parity harness | 16 |
| Normal migration activation | 17 |
| Real Miyoo migration validation | 18 |
| Journal/synchronous benchmark | 19–20 |
| SeriesScreen | 21 |
| EpisodeBrowser | 22 |
| DownloadManager planning | 23 |
| Home hierarchy | 24 |
| Hierarchy checkpoint | 25 |
| Remove whole-catalog RAM snapshot | 26 |
| Hierarchy-only hardware benchmark | 27 |
| LibraryCache/Home schema+import+parity | 28–30 |
| Startup/Home authoritative DB switch | 31 |
| Lazy indexed Home reads | 32 |
| Home/library hardware benchmark | 33 |
| Final legacy retirement | 34 |

## Dependency/Allowed-Files re-audit

The second audit intentionally keeps integration late:

- Tasks 02–16 do not wire CatalogDb into normal App session lifecycle except App ownership shell where explicitly allowed.
- Scope semantics are unit-proven at Tasks 04–05 before schema/DAL/migration code depends on them.
- Only Task 17 edits App session/login/logout integration to activate scoped migration.
- Consumer screen/DownloadManager Allowed Files begin only after real migration + journal gates.
- LibraryCache/Home edit scope remains after CP-F.
- Hardware-only tasks write factual `docs/sqlite-migration-benchmark.md`; they do not silently patch application code.
- Any hardware failure requiring code changes is a STOP and roadmap amendment/focused task, not opportunistic scope expansion.

## CP-A footprint evidence

CP-A additionally requires:

- ARM `miyoofin` executable bytes before SQLite vs after Task 07;
- packaged MiyooFin size before vs after Task 07;
- idle RSS before vs after CatalogDb service initialization **if practical**. If not practical, record `NOT PERFORMED` plus reason.

No size/RSS value may be invented.
