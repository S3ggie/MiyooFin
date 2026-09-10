# SQLite Migration Status — v3

Initial state: **ROADMAP APPROVED; IMPLEMENTATION NOT STARTED**

States:

- `[ ]` Not started
- `[~]` In progress
- `[x]` Completed, validated, and committed
- `[!]` Blocked
- `[H]` Mandatory human checkpoint

## Tasks

- [ ] 01 Vendor/build pinned SQLite 3.53.4 + capture pre/post initial ARM/package size
- [ ] 02 CatalogDb service lifecycle
- [ ] 03 Bounded priority queue/job lifecycle
- [ ] 04 CatalogDb server/user scope + epoch lifecycle
- [ ] 05 Scoped SQLite connection/prepared statements
- [ ] 06 Schema v1 bootstrap
- [ ] 07 Schema open/version policy + final CP-A size/RSS evidence
- [H] **CP-A — SQLite build/core/scope/schema**
- [ ] 08 MediaItem scalar codec
- [ ] 09 Genres/image-tags codec/parity
- [ ] 10 Hierarchy indexed read DAL
- [ ] 11 Atomic series-subtree UPSERT
- [ ] 12 Authoritative hierarchy reconciliation
- [ ] 13 CatalogDb telemetry via explicit MFT v2
- [ ] 14 Fresh scoped SQLite bootstrap/rebuild staging
- [ ] 15 Jellyfin-driven initial population/reconciliation
- [ ] 16 DownloadStore-driven offline hierarchy reconstruction
- [H] **CP-B — Fresh bootstrap/reconciliation/offline-download behavior**
- [ ] 17 Automatic fresh-database activation on normal valid scope configuration
- [ ] 18 Real Miyoo fresh-bootstrap validation
- [H] **CP-C — Real-device fresh bootstrap**
- [ ] 19 Journal benchmark harness
- [ ] 20 Real Miyoo journal/synchronous benchmark + decision
- [H] **CP-D — Journal benchmark**
- [ ] 21 Switch SeriesScreen
- [ ] 22 Switch EpisodeBrowser
- [ ] 23 Switch DownloadManager planning
- [ ] 24 Switch Home hierarchy persistence
- [ ] 25 Move hierarchy completion checkpoint to SQLite
- [ ] 26 Remove whole-catalog RAM snapshot
- [H] **CP-E — Hierarchy consumer cutover**
- [ ] 27 Hierarchy-only SQLite hardware benchmark
- [H] **CP-F — Hierarchy-only hardware benchmark**
- [ ] 28 Schema v2 LibraryCache/Home metadata
- [ ] 29 LibraryCache snapshot seed
- [ ] 30 LibraryCache/Home parity harness
- [ ] 31 Switch startup/Home metadata reads to CatalogDb
- [ ] 32 Lazy indexed Home reads/paging
- [ ] 33 Home/library real-device benchmark
- [H] **CP-G — Home/library cutover**
- [ ] 34 Retire legacy catalog/cache/sync persistence
- [H] **CP-H — Final retirement**

## Current production SQLite profile before CP-D

```text
journal_mode=DELETE
synchronous=FULL
locking_mode=NORMAL
```

## CP-A footprint evidence

- Pre-SQLite ARM executable bytes:
- Post-Task-07 ARM executable bytes:
- Delta bytes / percent:
- Pre-SQLite package bytes:
- Post-Task-07 package bytes:
- Delta bytes / percent:
- Idle RSS baseline:
- Idle RSS with CatalogDb initialized:
- RSS measurement environment/method:
- If RSS not practical: `NOT PERFORMED — <reason>`

## Hardware profile decision

Fill only from real Task-20 evidence:

- Miyoo model:
- SD card:
- OnionOS/firmware:
- Commit:
- Selected journal/synchronous/locking:
- Process-kill recovery:
- Hard-power recovery:
- Hard-power test status if not performed:
- CP-D reviewer:
