# Approved Final Architecture

## 1. Scope

SQLite becomes MiyooFin's durable **catalog metadata** store in two separately proven phases:

1. hierarchy/offline navigation metadata;
2. LibraryCache/Home metadata and lazy indexed reads.

The first phase must be hardware-proven before the second starts.

## 2. Ownership

### CatalogDb owns

Eventually:

- series metadata;
- seasons;
- episodes;
- movies when LibraryCache/Home migrates;
- item genres;
- Jellyfin image-tag metadata;
- hierarchy relationships;
- hierarchy completeness/reconciliation metadata;
- library views and membership after the Home phase;
- Continue Watching / Recently Added membership after the Home phase;
- schema/catalog version;
- hierarchy completion checkpoint after Task 25.

### CatalogDb does not own

These remain separate and authoritative:

- `ImageCache` files and decoded-image state;
- `DownloadStore` manifests, indexes, `.part` files, HLS segments, and validation;
- `DownloadManager` queue/state;
- `PlaybackRequest` and playback result files;
- `OfflinePlaybackJournal`;
- session/server/device-identity files;
- performance telemetry traces and rotation;
- downloaded media bytes.

A catalog row never means bytes are locally playable. `DownloadManager::hasComplete()` remains the local playback authority.

## 3. Threading

One app-scoped `CatalogDb` service owns:

- one worker thread;
- one SQLite connection;
- prepared statements;
- schema migration;
- catalog read/write execution;
- a bounded priority queue.

The worker is owned and joined. No detached threads.

Only the CatalogDb worker may call SQLite APIs after service startup. SQLite objects (`sqlite3*`, `sqlite3_stmt*`) never cross that ownership boundary.

The SDL/UI thread may:

- enqueue bounded jobs without blocking;
- publish already-completed results;
- continue showing old/local state while a query is pending.

The SDL/UI thread may never:

- open a database;
- prepare/step/reset/finalize a statement;
- begin/commit/rollback;
- run integrity checks;
- migrate;
- checkpoint WAL;
- wait synchronously for a CatalogDb result;
- join a CatalogDb worker during normal frame processing.


## CatalogDb scope/session lifecycle

CatalogDb is app-scoped but its open database is server/user-scoped.

It begins unconfigured. `configureScope(serverUrl,userId)` derives the same scope identity as `LibraryCache::scopeKey`.

A requested scope has a monotonically increasing epoch. The caller-side request increments that epoch immediately, so old-scope results become unpublishable before slow worker close/open work finishes.

On the CatalogDb worker, the latest scope-control operation:

1. rejects/cancels stale queued epoch jobs;
2. suppresses stale result publication;
3. finalizes old prepared statements;
4. closes the old DB;
5. opens/migrates the latest scoped DB;
6. prepares statements;
7. marks only that epoch Ready.

A superseded `A -> B -> C` configure sequence may skip opening A/B when their control jobs are already stale.

`deconfigureScope()` is the logout/no-valid-session transition: it advances the epoch immediately and worker-closes the old DB. No ordinary DB job is valid while unconfigured.

There is never more than one open scoped connection, and changing server/account can never expose rows from a previous scope.

## Fresh-database activation before read cutover

After fresh-bootstrap, Jellyfin-reconciliation, and offline-download validation, normal App session lifecycle becomes the activation path:

```text
valid saved session / successful login
    ↓
nonblocking configureScope
    ↓
CatalogDb worker
    ├─ open supported final DB
    └─ OR create/promote a fresh empty DB through .migrating
         ↓
    Jellyfin reconciliation when network is available
         or
    DownloadStore minimum offline hierarchy reconstruction
```

The bootstrap path never parses or imports `cache/offline/<scope>/catalog.v1`. Before hierarchy consumer cutover, SQLite is prepared/validated while existing legacy readers may continue to serve the UI. Once the hierarchy consumers cut over, `catalog.v1` is only a preserved rollback artifact until final retirement.

Logout/account/server change deconfigures the old scope immediately.


## 4. Queue model

The queue must be bounded and priority-aware.

Minimum classes:

1. `InteractiveRead` — Series/Episode/Home data needed for current navigation.
2. `ForegroundMetadataWrite` — result persistence directly associated with a screen refresh.
3. `BackgroundSync` — hierarchy generation/reconciliation.
4. `Maintenance` — integrity checks, migration bookkeeping, benchmark-only maintenance.

Requirements:

- FIFO within a priority class;
- no unbounded vectors/deques of jobs;
- enqueue failure/backpressure is explicit;
- BackgroundSync cannot starve InteractiveRead;
- InteractiveRead cannot bypass an already-running SQLite transaction;
- cancellation/generation tokens are checked before expensive work and before result publication;
- database transaction atomicity is never broken merely to service a higher-priority job.

The first implementation should prefer simple bounded scheduling over sophisticated fairness. Telemetry decides whether tuning is needed.

## 5. Connection model

Initial and approved model: **one SQLite connection** owned by CatalogDb.

Do not add a second reader connection until hardware telemetry proves:

- queue latency from reads is materially user-visible; and
- the single connection is the cause; and
- the added page-cache/locking complexity is justified.

## 6. Prepared statements

Prepare once per CatalogDb connection and reuse with reset/clear-bindings.

At minimum cache statements for:

- item UPSERT;
- genre replacement;
- image-tag replacement;
- get seasons;
- get episodes;
- delete stale seasons;
- delete stale episodes;
- hierarchy state UPSERT;
- authoritative series reconciliation;
- checkpoint read/write;
- later: library views/membership/Home-row queries.

A schema migration invalidates/finalizes the statement cache before migration and rebuilds it afterward.

## 7. Transaction semantics

Natural atomic unit for hierarchy writes: one complete series subtree.

```text
BEGIN IMMEDIATE
  UPSERT series
  UPSERT returned seasons
  UPSERT returned episodes
  DELETE stale episodes for returned seasons
  DELETE stale seasons for the series
  update hierarchy_state
COMMIT
```

A series subtree must never be published partially.

A large statement loop may check cancellation/telemetry at bounded row intervals, but it must not split the visible transaction merely to service cancellation.

## 8. Checkpoint semantics

The current conservative watermark rule is preserved.

If generation G starts from checkpoint T0:

```text
series A commit succeeds
series B commit succeeds
series C fails
```

then checkpoint remains T0.

A future generation may process A/B again; idempotent UPSERTs make that cheap.

Only after the complete requested generation succeeds and is still current may the hierarchy completion checkpoint advance.

After Task 25, checkpoint data lives in SQLite so metadata/checkpoint durability share one database authority. It still advances in its own final generation-completion transaction, not per series.

## 9. Local-first guarantees

Cached results are immediately usable while network work proceeds.

Offline Series/Episode browsing performs local indexed queries on CatalogDb and filters playability through DownloadManager/DownloadSnapshot. Download metadata remains a fallback for complete downloads whose catalog hierarchy is unavailable.

Database corruption must not erase or invalidate downloaded bytes.

## 10. Two-phase storage migration

### Phase A — hierarchy-only SQLite

Replaces `OfflineCatalog` hierarchy authority first. A fresh database is populated from current Jellyfin metadata, with DownloadStore metadata supplying the minimum offline hierarchy for complete downloads when network data is unavailable.

`LibraryCache` remains unchanged during this phase.

Hardware proof is mandatory before Phase B.

### Phase B — LibraryCache/Home SQLite

Adds library view/membership/Home-row storage and migrates startup/Home reads to indexed queries.

Only after Phase B parity and hardware validation may legacy snapshot/cache code be retired.

## 11. No production dual-write

There is no state in which production permanently writes both legacy whole-file catalog and SQLite.

Permitted:

- retaining `catalog.v1` untouched as a rollback artifact during rollout;
- host/debug comparison against synthetic or current Jellyfin/DownloadStore projections;
- shadow reads for validation before consumer cutover;
- one-time fresh database bootstrap through a temporary SQLite database.

Not permitted:

- every production hierarchy update rewriting `catalog.v1` and SQLite;
- using two stores as co-authorities.

## 12. Filesystem/database location

Final scoped database:

```text
cache/library/<scope>/catalog.sqlite3
```

The old hierarchy cache may remain only as a rollback artifact:

```text
cache/offline/<scope>/catalog.v1
```

during rollout; it is never read by the SQLite bootstrap path.

The database runs from MiyooFin's SD-card app storage on OnionOS. Treat the storage as removable flash with FAT32 behavior and non-desktop durability characteristics.
