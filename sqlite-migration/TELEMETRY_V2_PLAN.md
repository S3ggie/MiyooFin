# MFT v2 Plan for CatalogDb Telemetry

MFT v1 is frozen. `telemetry/SCHEMA_V1.md` must not be edited by this migration.

The current v1 contract has exact record types and exact WorkerId values. CatalogDb does not have a valid identity there, and reusing another worker or stuffing database timings into unrelated v1 records would be semantic abuse.

Therefore Task 13 deliberately introduces **MFT v2**.

## Compatibility model

- v1 trace header: `schema_version=1` — decoded exactly as today.
- v2 trace header: `schema_version=2`.
- v2 retains record types 1–14 with their existing layouts/meanings.
- v2 adds a CatalogDb WorkerId and one v2-only fixed `CatalogDbSummary` record.
- Decoder/analyzer accepts both versions.
- Existing v1 golden traces must still decode after Task 13.

## CatalogDbSummary semantics

Interval-only fixed numeric data:

- query count / total µs / max µs;
- transaction count / total µs / max µs;
- commit count / total µs / max µs;
- queue-wait total µs / max µs;
- enqueue rejection delta;
- rows inserted / updated / deleted;
- SQLite BUSY-family delta;
- SQLite IOERR-family delta;
- SQLite CORRUPT/NOTADB delta;
- reserved zero space.

Queue depth/high-water/activity/completed/failed/cancelled remain WorkerSample concepts using the new v2 CatalogDb worker identity.

No identifiers, paths, SQL strings, media metadata, or credentials are serialized.
