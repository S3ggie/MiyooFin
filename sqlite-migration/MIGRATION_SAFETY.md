# Migration Safety and Recovery

## Scope rule

Every migration operation belongs to one CatalogDb scope epoch.

Before promotion/readiness, the worker re-checks that the job epoch is still the latest requested epoch. A superseded scope may finish rollback/cleanup, but it may not publish Ready or data to the new account/server.

## Activation rule

After Task 17, migration is invoked automatically by the normal worker-side `configureScope(serverUrl,userId)` path for a valid saved/login session.

It is never invoked on the SDL/UI thread and does not require a hidden diagnostic command.

Before later consumer cutovers, the migrated SQLite DB is validated/shadow storage; legacy production reads remain unchanged.

## Core rule

The legacy OfflineCatalog is an immutable migration source.

Migration never edits:

```text
cache/offline/<scope>/catalog.v1
```

## File states

Authoritative new DB:

```text
cache/library/<scope>/catalog.sqlite3
```

Temporary import:

```text
cache/library/<scope>/catalog.sqlite3.migrating
```

A `.migrating` file is never opened as the production database.

## Migration state machine

```text
No final DB
  │
  ├─ no legacy catalog -> create empty SQLite DB
  │
  └─ legacy catalog exists
         ↓
     remove stale .migrating only after recognizing it as non-authoritative
         ↓
     create fresh .migrating
         ↓
     create schema
         ↓
     import
         ↓
     semantic validation
         ↓
     PRAGMA quick_check / integrity policy
         ↓
     PRAGMA foreign_key_check
         ↓
     close DB cleanly
         ↓
     ensure no live journal/WAL family remains
         ↓
     fsync completed file
         ↓
     rename .migrating -> catalog.sqlite3
         ↓
     reopen final DB and verify metadata/version
```

The legacy catalog remains untouched throughout.

## Existing final DB

If `catalog.sqlite3` exists:

- validate application ID;
- inspect user_version;
- let SQLite perform normal journal/WAL recovery;
- do not re-import over it automatically;
- do not remove sidecar journal/WAL files before opening.

If final DB is valid and supported, it is authoritative.

If it is corrupt, follow corruption policy below.

## Interrupted migration

At next startup, if:

```text
catalog.sqlite3.migrating
```

exists but final DB does not, treat `.migrating` as disposable incomplete state and rebuild it from the untouched legacy source.

If both exist, final DB wins; do not replace it with `.migrating`.

## Atomicity limits

`rename()` is necessary but do not assume desktop-ext4 semantics on the FAT32 SD card.

Migration hardware validation must include:

- interruption before import commit;
- interruption after import commit but before rename;
- restart after final rename;
- process kill during migration;
- controlled real power interruption only with explicit human approval and protected/backup media.

## Semantic validation before switch

Minimum checks:

- row counts by kind;
- every legacy series ID present;
- season membership/order;
- episode membership/order;
- scalar `MediaItem` equality;
- ordered genres;
- image tags;
- no FK violations;
- schema/application ID/version;
- hierarchy completeness;
- DB reopen succeeds.

`quick_check` alone is insufficient for semantic parity.

## Corruption handling

First allow SQLite's normal recovery by opening the full database family.

Never pre-delete:

```text
catalog.sqlite3-journal
catalog.sqlite3-wal
catalog.sqlite3-shm
```

If open/query reports corruption/not-a-database or validation fails:

1. close DB;
2. record safe error category/result codes;
3. quarantine the DB family under a non-authoritative name if possible;
4. do not touch downloaded media;
5. create fresh catalog DB;
6. rebuild from Jellyfin when online;
7. keep download metadata as offline fallback.

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
- preserve downloads/session files;
- report incompatibility.

## LibraryCache migration later

The LibraryCache/Home migration repeats the same principles:

- import old snapshot read-only;
- compare old/new projections;
- do not retire `snapshot.v1` until CP-G;
- no permanent dual-write.
