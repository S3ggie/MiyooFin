# Mandatory Human Review Checkpoints — v3

## CP-A — SQLite build/core/scope/schema

After Task 07. STOP.

Verify:

- official SQLite 3.53.4 pin/checksums;
- host/ARM exact same vendored `sqlite3.c`;
- SQLite separate C object compiled `-Os`;
- exact approved compile macros and no speculative OMIT flags;
- one app-scoped CatalogDb worker, owned/joined;
- bounded priority queue;
- explicit server/user scope lifecycle;
- no DB open before valid scope;
- scope epoch immediately invalidates old results;
- scope switch finalizes statements/closes old DB before new DB opens;
- logout/deconfigure leaves no scoped DB;
- one SQLite connection only;
- no SQLite calls on SDL/UI;
- schema v1/version policy correct;
- runtime behavior still unchanged (normal App fresh-database activation is intentionally later Task 17).

### Mandatory footprint review

Compare exact:

- pre-SQLite ARM executable bytes vs post-Task-07 bytes;
- pre-SQLite package bytes vs post-Task-07 package bytes.

Also review idle RSS before vs after CatalogDb service initialization **if practical**. If not practical, the executor must say `NOT PERFORMED` with a reason; absence is not a pass claim.

Approval required before Task 08.

## CP-B — Fresh bootstrap/reconciliation/offline downloads

After Task 16. STOP.

Verify:

- scope-aware bootstrap/reconciliation cannot cross server/user epochs;
- no CatalogDb path parses or imports `catalog.v1`;
- `.migrating` is only a disposable fresh-DB rebuild file and never authoritative;
- fresh schema bootstrap and supported final-DB open are safe;
- Jellyfin subtree reconciliation validates current authoritative metadata, genres, image tags, hierarchy/order;
- DownloadStore offline reconstruction preserves complete downloaded-media visibility/playability and marks hierarchy incomplete;
- quick/integrity policy + FK check;
- interrupted fresh-DB rebuild is safely retryable;
- no production dual-write;
- no normal App activation yet (Task 17 is the reviewed activation step).

## CP-C — Real-device fresh bootstrap

After Task 18. STOP.

Requires actual Miyoo Mini Plus evidence.

Verify fresh bootstrap was triggered by **normal valid saved-session launch or normal login through Task-17 configureScope**, not a hidden diagnostic path.

Verify:

- tested commit/build/environment recorded;
- no legacy catalog import occurred;
- fresh final DB was created and populated from current Jellyfin metadata when online;
- offline first launch retained browsable/playable complete downloads through DownloadStore reconstruction;
- duration/CPU/RSS/process I/O/final DB size recorded;
- process-interrupted fresh-DB rebuild/recovery;
- `catalog.v1`, if present, remains untouched as a rollback artifact;
- clean restart/reopen;
- scope/logout stale-result behavior exercised;
- evidence committed to `docs/sqlite-migration-benchmark.md`.

## CP-D — Journal benchmark

After Task 20. STOP.

Requires actual Miyoo evidence for A–F candidate matrix.

Human must explicitly **approve or reject the evidence-backed profile committed by Task 20**. If results were ambiguous, Task 20 must have conservatively retained DELETE/FULL. If CP-D rejects the committed choice, stop and add a focused follow-up task before Task 21.

No WAL selection by assumption.

Review recovery, write amplification, timings, RSS, file sidecars, checkpoint correctness, and any hard-power test status.

## CP-E — Hierarchy consumer cutover

After Task 26. STOP.

Verify SeriesScreen, EpisodeBrowser, DownloadManager planning, Home hierarchy, and checkpoint use CatalogDb and enforce current scope epoch.

Verify whole-catalog runtime RAM snapshot is gone and `catalog.v1` is not a SQLite bootstrap authority; any remaining pre-cutover legacy reader is limited to compatibility/rollback behavior.

## CP-F — Hierarchy-only hardware benchmark

After Task 27. STOP.

Real Miyoo evidence required before any LibraryCache/Home schema work.

Review sync duration, process reads/writes, CPU, RSS, Series/Episode loading, worker completion/cancellation, UI stalls, checkpoint correctness, offline browsing, and local playback.

## CP-G — Home/library cutover

After Task 33. STOP.

Real Miyoo + host parity required.

Verify bounded/lazy Home reads, exact organizational ordering, no stale-scope publication, local-first behavior, startup/navigation performance, and unchanged ImageCache/download/playback ownership.

## CP-H — Final retirement

After Task 34. STOP.

Verify no production legacy writer/runtime reader remains except explicitly approved rollback/compatibility handling. No `catalog.v1` importer is required.

Verify fresh install, old upgrade, SQLite install, corrupt DB recovery, logout/scope switching, offline/download/local/network playback, and final Miyoo smoke test.
