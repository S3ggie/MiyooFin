# MFT v2 — CatalogDb telemetry extension

MFT v2 uses the same little-endian `MFT1` container and 80-byte header as v1,
with `schema_version=2`. The common record header and v1 record types 1–14
are copied unchanged. A v1 trace is decoded using the v1 rules and remains
byte-for-byte compatible.

## File header

Offsets, field sizes, and meanings are identical to `SCHEMA_V1.md`; only the
schema version at offset 4 is 2. Header reserved fields remain zero.

## Existing records

Record types 1–14 retain the exact v1 layouts and meanings. In v2, record type
4 may additionally carry worker ID 15, which is the v2-only `CatalogDb`
worker. The v1 worker IDs 1–14 are unchanged. The v1 UI-stall worker mask is
unchanged; the CatalogDb worker is not a UI-stall worker and therefore has no
mask bit.

## Record type 15 — CatalogDbSummary — payload 112, total 128

The service thread emits one fixed record per aggregate interval. All counters
are interval deltas and all durations are microseconds.

|Off|Size|Type|Field|
|---:|---:|---|---|
|16|4|u32|query_count|
|20|8|u64|query_total_us|
|28|4|u32|query_max_us|
|32|4|u32|transaction_count|
|36|8|u64|transaction_total_us|
|44|4|u32|transaction_max_us|
|48|4|u32|commit_count|
|52|8|u64|commit_total_us|
|60|4|u32|commit_max_us|
|64|8|u64|queue_wait_total_us|
|72|4|u32|queue_wait_max_us|
|76|4|u32|enqueue_rejected_delta|
|80|4|u32|rows_inserted|
|84|4|u32|rows_updated|
|88|4|u32|rows_deleted|
|92|4|u32|sqlite_busy_family_delta|
|96|4|u32|sqlite_ioerr_family_delta|
|100|4|u32|sqlite_corrupt_notadb_delta|
|104|4|u32|reserved0=0|
|108|4|u32|reserved1=0|
|112|8|u64|reserved2=0|
|120|8|u64|reserved3=0|

The three SQLite error counters classify primary result codes in the BUSY,
IOERR, and CORRUPT/NOTADB families. Queue depth, queue high-water, active,
completed, failed, and cancelled are represented by the v2 CatalogDb
WorkerSample identity rather than duplicated here. `rows_inserted` counts
successful new-row upsert statements, `rows_updated` counts successful
existing-row upsert statements, and `rows_deleted` counts rows removed by
reconciliation or stale hierarchy replacement. These are producer-side
statement deltas; a later transaction rollback is not represented separately.

## Compatibility and privacy

Decoders accept schema versions 1 and 2. Unknown records are skipped when
their bounded record size is valid; impossible sizes and malformed known
records stop decoding safely. A partial tail does not discard prior complete
records. The v2 additions contain numeric counters and monotonic timings only:
no IDs, titles, URLs, paths, SQL text, scope keys, or credentials are written.

When runtime telemetry is disabled, CatalogDb telemetry calls are no-ops and
no telemetry file or serialization work is created. Benchmark DB/WAL/journal
sizes remain benchmark-tool outputs outside MFT.
