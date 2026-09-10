# Fresh SQLite Bootstrap and Recovery Safety

## Ownership and scope

Every bootstrap, rebuild, reconciliation, and recovery operation belongs to one
CatalogDb scope epoch.

Authoritative sources are deliberately separate:

- Jellyfin is authoritative for server metadata and hierarchy when network data
  is available.
- DownloadStore/DownloadManager are authoritative for downloaded media, durable
  download metadata, and local playability.
- Playback/download state remains separately authoritative.
- ImageCache remains separate from CatalogDb.
- `cache/offline/<scope>/catalog.v1` is reconstructible metadata cache state. It
  is not a SQLite bootstrap input or production authority.

Before promotion/readiness, the worker re-checks that the job epoch is still the
latest requested epoch. A superseded scope may finish rollback/cleanup, but it
may not publish Ready or data to the new account/server.

## Activation rule

After Task 17, fresh database activation is invoked automatically by the normal
worker-side `configureScope(serverUrl,userId)` path for a valid saved/login
session. It is never invoked on the SDL/UI thread and does not require a hidden
diagnostic command.

For a valid current scope:

1. If a supported final `catalog.sqlite3` exists, open it and allow SQLite's
   normal journal/WAL recovery.
2. If no final DB exists, create a fresh empty schema database using the
   disposable `.migrating` file, validate it, and promote it atomically.
3. If network metadata is available, reconcile current Jellyfin hierarchy in
   bounded complete-subtree transactions.
4. If the first launch is offline, seed only the minimum hierarchy needed to
   browse complete DownloadStore items from authoritative durable download
   metadata.
5. Keep `hierarchy_state.complete=0` until a successful Jellyfin reconciliation
   completes the relevant hierarchy generation.

The bootstrap path must never parse, import, rewrite, or delete `catalog.v1`.

Before hierarchy consumer cutover, existing legacy readers may continue serving
the UI as a compatibility path. After the hierarchy cutover, `catalog.v1` may
remain untouched only as a rollback artifact until final retirement.

## File states

Authoritative new DB:

```text
cache/library/<scope>/catalog.sqlite3
```

Temporary fresh bootstrap/rebuild:

```text
cache/library/<scope>/catalog.sqlite3.migrating
```

A `.migrating` file is never opened as the production database and never
contains an import from `catalog.v1`.

## Fresh bootstrap state machine

```text
Scope configured
      │
      ├─ supported final DB exists
      │       ↓
      │   open/recover final DB
      │
      └─ final DB absent
              ↓
          inspect disposable temp DB family
              ↓
          discard incomplete temp state
              ↓
          create fresh .migrating schema DB
              ↓
          structural/schema validation
              ↓
          close, fsync, rename to catalog.sqlite3
              ↓
          reopen final DB and verify metadata/version
              ↓
          Jellyfin reconciliation when online
          or DownloadStore offline reconstruction
```

If both final and `.migrating` exist, final wins. The temp file is a cleanup
candidate only after the final DB has been opened and validated. Never replace a
valid final DB with a temp file.

## Interrupted fresh bootstrap/rebuild

If `.migrating` exists but the final DB does not, treat the temp database as
disposable incomplete state. Close any recoverable connection, remove only that
temporary DB family, and rebuild a fresh empty database. No legacy source is
needed for recovery.

If a process is killed during Jellyfin reconciliation, SQLite recovery must leave
the last committed subtree authoritative. Retry the incomplete generation from
Jellyfin; do not advance the hierarchy checkpoint until the complete generation
has succeeded.

Controlled real power interruption requires explicit human approval and
protected/disposable media.

## Existing final DB

If `catalog.sqlite3` exists:

- validate application ID and supported `user_version`;
- let SQLite perform normal journal/WAL recovery;
- do not pre-delete journal, WAL, or SHM sidecars before opening;
- if valid and supported, use it without consulting `catalog.v1`;
- do not replace it with a fresh DB merely because `catalog.v1` exists.

If the final DB is corrupt or unsupported, follow the corruption policy below.

## Semantic validation

Bootstrap validation covers:

- schema/application ID/version;
- required pragmas and foreign keys;
- `PRAGMA quick_check` and `PRAGMA foreign_key_check`;
- final DB reopen;
- current Jellyfin counts/order/fields for the response being reconciled;
- DownloadStore-backed offline hierarchy visibility and complete-download
  playability;
- `hierarchy_state.complete=0` before successful full Jellyfin reconciliation;
- checkpoint advancement only after a complete committed generation.

There is no required full-catalog comparison against `catalog.v1`, and no
duplicate-ID canonicalization rule for legacy cache rows.

## Corruption handling

First allow SQLite's normal recovery by opening the full database family.

Never pre-delete:

```text
catalog.sqlite3-journal
catalog.sqlite3-wal
catalog.sqlite3-shm
```

If open/query reports corruption, not-a-database, or validation failure:

1. close the DB;
2. record safe error category/result codes;
3. quarantine the DB family under a non-authoritative name if possible;
4. do not touch downloaded media, manifests, playback state, or `catalog.v1`;
5. create a fresh empty catalog DB;
6. rebuild from Jellyfin when online;
7. reconstruct the minimum downloaded hierarchy from DownloadStore metadata when
   offline.

Do not implement on-device B-tree salvage.

## Future schema migration

For supported `N -> N+1`:

```text
finalize statement cache
BEGIN IMMEDIATE
perform DDL/data migration
set user_version=N+1
COMMIT
rebuild statement cache
```

On any failure, rollback and leave the previous supported schema authoritative.

If `user_version` is newer than the application supports:

- do not modify it;
- fail catalog open safely;
- preserve downloads/session/files/rollback artifacts;
- report incompatibility.

## LibraryCache/Home migration later

The LibraryCache/Home phase remains separate from the hierarchy bootstrap:

- Tasks 28–30 handle schema-v2 LibraryCache snapshot seeding and projection
  parity; this is not `catalog.v1` import;
- do not retire `snapshot.v1` until CP-G;
- no permanent dual-write;
- do not delete `catalog.v1` or other rollback artifacts until the final approved
  retirement task.
