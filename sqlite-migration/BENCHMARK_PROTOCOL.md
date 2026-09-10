# Miyoo Mini Plus Benchmark Protocol

## Purpose

Choose SQLite durability/performance settings and prove fresh SQLite bootstrap, reconciliation, and hierarchy operation are practical on real hardware.

Hardware evidence is mandatory for Tasks 18, 20, 27, and 33.

## Environment record

Every run set must record:

```text
date/time
git commit
Miyoo model
OnionOS version if available
firmware version if available
SD-card make/model/capacity if known
free storage before run
database size
legacy `catalog.v1` artifact size if present (record-only; never imported)
library counts: series/seasons/episodes/movies as applicable
network route/server condition
telemetry enabled/disabled state
journal/synchronous/locking profile
```

Do not compare runs from materially different library/server states without noting the difference.

## Existing telemetry to use

MFT v1 already provides:

- process CPU;
- RSS/peak RSS;
- process read/write bytes;
- free storage;
- frame timing;
- UI stalls;
- worker queue/activity/completion/failure/cancellation;
- network request duration/bytes;
- library sync duration;
- download behavior.

CatalogDb telemetry added in **Task 13 via explicit MFT v2** must add:

- CatalogDb queue depth/high-water;
- job queue-wait time;
- query count/total/max duration;
- transaction count/total/max duration;
- commit duration;
- rows inserted/updated/deleted;
- SQLite busy/I/O/corruption counters;
- optional benchmark-only DB/journal/WAL file-size samples;
- WAL checkpoint duration/count only if WAL is under test.

No IDs/titles/SQL strings/paths in performance records.

MFT v1 remains frozen. Real-device SQLite benchmark traces should be MFT v2 once Task 13 lands; the analyzer must continue decoding historical v1 baseline traces.

## Journal matrix

Run on the same Miyoo and same SD card:

| ID | journal | synchronous | locking |
|---|---|---|---|
| A | DELETE | FULL | NORMAL |
| B | DELETE | EXTRA | NORMAL |
| C | WAL | FULL | NORMAL |
| D | WAL | NORMAL | NORMAL |
| E | WAL | FULL | EXCLUSIVE |
| F | WAL | NORMAL | EXCLUSIVE |

Initial production baseline remains A until CP-D.

SQLite documentation says **"WAL is significantly faster in most scenarios"** and notes more sequential I/O/fewer fsyncs. Source: https://www.sqlite.org/wal.html . This is motivation for testing, not a result for Miyoo.

SQLite documentation also states that WAL with `synchronous=NORMAL` can lose recent durability after power loss while remaining protected from corruption. Source: https://www.sqlite.org/pragma.html#pragma_synchronous . The benchmark must evaluate whether that trade is acceptable with MiyooFin's conservative checkpoint.

## Workloads

### W1 — open + first local hierarchy query

- clean app launch;
- open DB;
- query one known series;
- query one known season.

Measure open/query time and UI first-use behavior.

### W2 — typical series transaction

Representative:

- 1 series;
- 1–3 seasons;
- 10–50 episodes.

Repeat enough times using controlled fixture/replay data to get stable distributions.

### W3 — large series transaction

Use a high-episode-count series. Record exact counts.

### W4 — changed hierarchy generation

Run generations of approximately:

- 1 affected series;
- 5 affected series;
- 20 affected series;

or nearest realistic sets available.

### W5 — authoritative deletion

Validate removal of:

- episode;
- season;
- entire series through top-level reconciliation.

### W6 — repeated navigation

Repeated async:

- Series open/back;
- EpisodeBrowser open/back;
- season changes.

Measure DB queue latency and UI stalls.

### W7 — fresh bootstrap and initial population

- Start with no final SQLite DB in an isolated scope.
- Create/promote a fresh empty DB through the normal worker path.
- With network available, reconcile a controlled current Jellyfin hierarchy.
- With network unavailable, reconstruct the minimum hierarchy needed for complete DownloadStore items.
- Confirm `catalog.v1`, if present, is not read or modified by the bootstrap path.

### W8 — restart/recovery

Clean shutdown, process kill, and interrupted transaction recovery.

## Repetition

For timing comparisons:

- discard obvious setup run only if documented;
- minimum 5 measured runs per profile/workload;
- prefer 10 for high-variance SD-card writes;
- report median plus p95/max or the complete sample set when N is small.

Do not select a profile from a single run.

## Power/interruption tests

Two levels:

### Level 1 — mandatory process interruption

Use process termination during controlled bootstrap/reconciliation writes. Confirm restart/recovery and checkpoint behavior.

### Level 2 — controlled power interruption

Requires explicit human approval because it risks SD-card filesystem damage.

Use backup/disposable or fully recoverable test media where practical.

For finalist profiles, interrupt at randomized points during:

- active series transaction;
- generation finalization;
- fresh DB bootstrap/rebuild and initial reconciliation;
- WAL checkpoint if applicable.

After reboot:

```text
open database
validate application_id/user_version
PRAGMA quick_check
PRAGMA foreign_key_check
verify checkpoint <= fully committed generation
verify complete downloads remain present/playable
```

Never claim hard-power validation if only process kill was used.

## Decision criteria for CP-D

A winning profile must have:

1. no observed corruption in required interruption tests;
2. correct generation/checkpoint behavior;
3. acceptable DB/journal sidecar cleanup/recovery on FAT32;
4. lower or acceptably equal write amplification;
5. no harmful RSS increase;
6. no regression in UI stalls;
7. better overall hierarchy workload time, or a documented durability reason for accepting a slower profile.

If results are ambiguous, keep DELETE+FULL and gather more data.

## Hierarchy-only A/B at CP-F

Compare the post-Task-24 SQLite build against the preserved telemetry baseline from the current optimization base.

Required metrics:

- full/changed hierarchy sync duration;
- process write bytes;
- process read bytes;
- CPU;
- RSS/peak RSS;
- Series cached loading;
- EpisodeBrowser cached loading;
- hierarchy completed/failed/cancelled counts;
- UI stalls;
- startup;
- download planning;
- offline browsing and local playback functional checks.

The hierarchy change is not considered successful merely because SQL transactions are individually faster.

## Home/library A/B at CP-G

After lazy Home migration compare:

- cold/warm startup;
- time to usable cached Home;
- Movies tab first display;
- Shows tab first display;
- alphabet-filter navigation;
- RSS/peak RSS;
- process reads;
- UI stall counts;
- DB query/queue latency.

## Storage artifacts

Record sizes for:

```text
catalog.sqlite3
catalog.sqlite3-journal
catalog.sqlite3-wal
catalog.sqlite3-shm
legacy `catalog.v1` / LibraryCache snapshot artifacts
```

only in benchmark tooling/logs. Do not emit private paths or media IDs into MFT telemetry.


## Scope-isolation scenarios

At real-device gates that exercise account/session lifecycle:

- configure scope A, start/queue catalog work, then logout -> no late A result may publish;
- configure A then switch server/account to B -> B must never display/query A rows;
- rapid A -> B -> C -> only C becomes Ready/publishable;
- close/reopen same scope -> expected rows return only after that scope is Ready.

These are correctness tests first; do not treat them as performance-only measurements.
