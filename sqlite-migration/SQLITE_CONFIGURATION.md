# SQLite Configuration Contract

## Version pin

Pin official SQLite **3.53.4**, version number `3530400`.

Official release artifact:

```text
sqlite-amalgamation-3530400.zip
```

Official download-page SHA3-256 for the archive:

```text
628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
```

Official release-history SHA3-256 for `sqlite3.c`:

```text
67f423e9ebbbdc473cbc4772c872ee6b89f31fde4ed0279a5c25d5f65c043a16
```

Record both during vendoring. Do not substitute a distro copy.

Sources:

- https://sqlite.org/download.html
- https://www.sqlite.org/changes.html

Official download-page wording: **"C source code as an amalgamation, version 3.53.4."**

## Same source for both targets

Host and ARM must compile the exact same vendored:

```text
vendor/sqlite/sqlite3.c
vendor/sqlite/sqlite3.h
```

No host `pkg-config sqlite3`.  
No OnionOS system `libsqlite3.so`.  
No `-lsqlite3`.

## Compile separately

SQLite must be its own C compilation unit.

Use C compiler, not C++ compiler.

Optimization:

```text
-Os
```

Do not make SQLite inherit MiyooFin's host `-O0` or ARM `-O2` by accident.

## Required initial macros

Exactly these architecture-driven options:

```text
-DSQLITE_THREADSAFE=2
-DSQLITE_DEFAULT_MEMSTATUS=0
-DSQLITE_DQS=0
-DSQLITE_TRUSTED_SCHEMA=0
-DSQLITE_OMIT_LOAD_EXTENSION
```

Do not add speculative feature-removal macros during the first migration.

In particular, do not add without a separately approved size experiment:

```text
SQLITE_OMIT_WAL
SQLITE_OMIT_FOREIGN_KEY
SQLITE_OMIT_INTEGRITY_CHECK
SQLITE_OMIT_PRAGMA
SQLITE_OMIT_UTF16
SQLITE_OMIT_JSON
SQLITE_OMIT_SHARED_CACHE
SQLITE_UNTESTABLE
```

Some may eventually be safe; they are deliberately outside this migration.

Official compile-option documentation states for `SQLITE_THREADSAFE=2` that SQLite may be used in a multithreaded program as long as two threads do not use the same connection simultaneously. The roadmap makes this stronger: only CatalogDb's worker owns the connection.

Source: https://www.sqlite.org/compile.html

## Initial connection pragmas

On each connection, verify/set:

```sql
PRAGMA foreign_keys = ON;
PRAGMA trusted_schema = OFF;
PRAGMA journal_mode = DELETE;
PRAGMA synchronous = FULL;
```

Also set/verify the approved application ID and schema `user_version`.

The application ID must be checked against SQLite's current magic-number registry during Task 05 before selecting the numeric value. Do not silently invent a colliding value.

## Do not tune yet

Until evidence exists, do not override:

```text
page_size
cache_size
temp_store
mmap_size
wal_autocheckpoint
journal_size_limit
cache_spill
auto_vacuum
```

Document defaults observed on host and Miyoo, but do not cargo-cult tune.

## Journal decision

Before CP-D, production baseline remains:

```text
DELETE + synchronous=FULL + locking_mode=NORMAL
```

Benchmark candidates:

1. DELETE + FULL + NORMAL locking
2. DELETE + EXTRA + NORMAL locking
3. WAL + FULL + NORMAL locking
4. WAL + NORMAL + NORMAL locking
5. WAL + FULL + EXCLUSIVE locking
6. WAL + NORMAL + EXCLUSIVE locking

Official SQLite WAL documentation says **"WAL is significantly faster in most scenarios"**, but that is a hypothesis for the Miyoo SD card, not permission to select WAL.

Source: https://www.sqlite.org/wal.html

Official synchronous documentation says WAL with `synchronous=NORMAL` is safe from corruption but may lose recent durability after power loss. That tradeoff is acceptable only if real-device results and checkpoint behavior justify it.

Source: https://www.sqlite.org/pragma.html#pragma_synchronous

SQLite also documents that setting EXCLUSIVE locking before first WAL access can avoid creating the shared-memory wal-index. This makes the EXCLUSIVE WAL variants worth testing on the one-connection Miyoo architecture.

Source: https://www.sqlite.org/wal.html

## Runtime verification

Tests should expose a safe internal diagnostic that verifies:

- `sqlite3_libversion_number() == 3053004`;
- `sqlite3_threadsafe() == 2`;
- required compile options are present;
- forbidden/speculative OMIT flags were not accidentally introduced;
- foreign keys are actually on;
- trusted schema is actually off;
- active journal/synchronous profile matches the build/runtime decision.

Do not send compile-option strings to performance telemetry.


## Scope/open rule

These pragmas/configuration apply only after CatalogDb has a valid current server/user scope.

No SQLite database is opened before a valid scope exists. Scope switch/deconfigure is worker-owned and closes the old connection before another scoped connection is opened.
