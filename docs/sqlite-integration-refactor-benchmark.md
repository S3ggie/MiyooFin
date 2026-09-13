# SQLite integration refactor Miyoo performance rebaseline

Status: **COMPLETE — six valid physical-device runs**

This is an evidence-only Task 33 record. No production optimization or
durability change was made.

## Environment and build identity

| Field | Value |
|---|---|
| Date | 2026-09-13 |
| Branch / committed HEAD | `audit/sqlite-integration-current` / `d1643d4309288cbfa68cca3859ee73eb59647c2d` |
| Device | Miyoo Mini Plus, `armv7l`, Linux `4.9.84` |
| SSH/deployment | Developer SSH service, port 2222; package transfer to SD-card temporary paths, SHA-256 verification, atomic rename |
| Application binary SHA-256 | `40dbf95c38756d7a17d0a35ba064306c4c13d03068b4c0160433f0c98bb4b16f` |
| `playback_runner.sh` SHA-256 | `9ab4f28e7e5b563a2f3cd0ab854a38128991bb45d0966c9cc86fbb146ef31442` |
| Runtime route | Public Jellyfin path for every recorded request; zero fallback attempts |
| Catalog scope | `907a3789cc02e9e4`; same configured account/server and comparable Wi-Fi conditions |
| Final catalog | `2,547,712` bytes; schema `user_version=3`; `PRAGMA quick_check` returned `ok`; `media_items=1,402`; `library_membership=1,402` |

The binary embeds the post-Task-32 commit identity. The deployment was built
from that HEAD plus the pre-existing user worktree changes that were present
throughout this evidence collection; those changes were not altered or staged.
All six runs used the same resulting verified package. No claim is made that
these measurements are a clean-worktree comparison against an earlier build.

## Run matrix

Cold means the active SQLite catalog was moved to an explicit recoverable
backup directory before launch. Warm means the regenerated catalog remained in
place. Each run was launched with `MIYOOFIN_TELEMETRY=1`, observed for the same
runtime window, and ended through the validated graceful-exit helper. The
three cold and three warm runs therefore completed the required matrix.

Times are seconds from the telemetry process start. `first page` is the
explicit `first_bounded_page_ready` marker for cold runs. Warm startup does not
emit that marker because cached Home is already interactive; its
`first_page_persisted` time is recorded as the available bounded-page proxy.

| Run | Catalog | Start → scope ready | Warm committed query ready | First useful Home frame | First page / persisted proxy | Full LibrarySync completion | Sync duration | Items / views |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| cold-1 | fresh | 4.734 | n/a | 9.353 | 9.353 | 39.171 | 34.612 | 1,402 / 3 |
| cold-2 | fresh | 1.445 | n/a | 5.084 | 5.084 | 28.858 | 27.519 | 1,402 / 3 |
| cold-3 | fresh | 1.111 | n/a | 4.852 | 4.743 | 30.516 | 29.574 | 1,402 / 3 |
| warm-1 | existing | 1.975 | 1.867 | 1.975 | 5.825 (persisted) | 33.603 | 31.823 | 1,402 / 3 |
| warm-2 | existing | 1.098 | 0.993 | 1.098 | 11.448 (persisted) | 37.847 | 36.907 | 1,402 / 3 |
| warm-3 | existing | 0.987 | 0.987 | 1.094 | 5.484 (persisted) | 32.007 | 31.113 | 1,402 / 3 |

All six `LibrarySync` records reported `outcome=Success`, `request_count=32`,
`media_count=1,402`, and `views_count=3`.

## Network and SQLite evidence

The sync request profile was stable: one token validation, one views request,
one resume-items request, one latest-items request, 30 bounded library-item
requests, and 29 download-playback-info requests. Every decoded request used
the Public route and had zero fallback attempts. The principal request
latencies were:

| Run | ResumeItems ms | LatestItems ms | Views ms | LibraryItems average / max ms |
|---|---:|---:|---:|---:|
| cold-1 | 1,960.2 | 903.7 | 719.1 | 676.8 / 927.6 |
| cold-2 | 1,447.7 | 833.2 | 693.9 | 575.0 / 746.6 |
| cold-3 | 1,551.0 | 728.7 | 599.9 | 664.4 / 827.9 |
| warm-1 | 1,695.5 | 689.0 | 630.6 | 575.7 / 816.1 |
| warm-2 | 8,065.9 | 869.8 | 617.9 | 617.3 / 901.7 |
| warm-3 | 1,501.1 | 739.9 | 621.2 | 590.1 / 885.3 |

The binary CatalogDb summaries recorded the following bounded catalog-read
and queue-wait totals. Page transaction timing was additionally captured from
the millisecond-resolution CatalogDb diagnostics markers.

| Run | CatalogDb query count / total / max ms | Queue wait total / max ms | Page transactions | Mean begin → commit ms | `readMediaPage` dequeue / ready |
|---|---:|---:|---:|---:|---:|
| cold-1 | 2 / 3.5 / 1.9 | 174.0 / 88.0 | 30 | 282.2 | 2 / 2 |
| cold-2 | 2 / 3.2 / 1.6 | 166.0 / 83.9 | 30 | 187.3 | 2 / 2 |
| cold-3 | 2 / 3.7 / 2.0 | 89.5 / 45.8 | 30 | 161.2 | 2 / 2 |
| warm-1 | 2 / 51.4 / 41.3 | 82.2 / 61.8 | 30 | 330.0 | 2 / 2 |
| warm-2 | 2 / 32.9 / 22.4 | 48.0 / 25.6 | 30 | 245.9 | 2 / 2 |
| warm-3 | 2 / 48.9 / 38.6 | 96.7 / 53.6 | 30 | 300.6 | 2 / 2 |

The current telemetry summary aggregates CatalogDb read/queue counters but
does not expose a separate per-page transaction/commit counter. Accordingly,
the page transaction counts and begin-to-commit values above come from the
verified `[CatalogDb] page_transaction_begin/commit` diagnostics stream, not
from inferred binary summary totals.

## Artwork and frame/resource evidence

No remote Artwork request occurred in any of the six runs. The image cache was
already populated, so artwork work measured here is cache read plus decode,
not a cold artwork-download benchmark. Cache probes reported 1,402–1,411 hits,
zero misses, zero cache read/write failures, and zero decode failures per run.

| Run | Cache decodes | Decode total / max ms | Decodes overlapping page-commit window | Frame stalls >50 / >100 ms | Frame max ms | RSS peak KiB | CPU device average / peak % | Process CPU s |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| cold-1 | 16 | 16.1 / 1.4 | 7 | 1,946 / 50 | 150.8 | 17,004 | 40.29 / 72.40 | 155.633 |
| cold-2 | 16 | 42.1 / 7.4 | 14 | 1,620 / 4 | 117.3 | 16,852 | 42.70 / 69.05 | 132.302 |
| cold-3 | 16 | 25.3 / 10.9 | 16 | 1,636 / 3 | 100.5 | 16,784 | 42.70 / 68.40 | 133.148 |
| warm-1 | 16 | 17.4 / 1.4 | 0 | 1,436 / 1 | 100.5 | 16,456 | 42.99 / 69.38 | 133.129 |
| warm-2 | 10 | 9.0 / 1.0 | 0 | 1,312 / 2 | 100.5 | 16,188 | 42.14 / 67.67 | 131.377 |
| warm-3 | 16 | 19.7 / 2.1 | 0 | 1,665 / 2 | 100.5 | 16,832 | 42.55 / 66.53 | 131.831 |

“Overlap” is the count of cache decode records whose timestamps fell between
the first and last page-commit diagnostics marker. It is not a claim that
artwork downloads overlapped synchronization: there were no remote artwork
downloads in this matrix.

Process read/write counters were reported as zero on this device for every
run. They are recorded as **unavailable/unsupported by the device telemetry
source**, not as evidence of zero physical I/O. Free storage remained above
53.3 GB during the captures.

Telemetry health was clean for every trace: zero dropped records, zero writer
errors, and zero maximum queue depth at the analyzer level. Each trace decoded
and analyzed successfully. The traces were archived on-device as
`task33-verified-{cold,warm}-{1,2,3}.mft`; working copies and decoded analysis
were kept outside the repository.

## Compatibility and bottleneck classification

Existing benchmark documents describe different revisions, scenarios, or
conditions, so no historical numeric comparison is claimed. The six-run set
is internally comparable because it used one verified package, one device,
one route, one scope, and the same run procedure.

The evidence points primarily to **network/request latency**, with a secondary
bounded page-write cost. Sync completion is 27.5–36.9 seconds after the
LibrarySync work begins, while the Public-route request set dominates the
measured work. Catalog read query totals are low (3.2–51.4 ms per trace), and
page commits are visible but not the dominant end-to-end sync duration. Cached
artwork decoding is small relative to sync time. Frame stalls are measurable
on the device, but the first useful Home frame is available in 1.1–9.4 seconds
and no correctness regression appeared during these runs. No route fallback,
telemetry health, or catalog-integrity failure was observed.

## Decision

The final refactored architecture is measurable and healthy under this Task 33
matrix. No immediate optimization is justified by this evidence-only task.
If faster synchronization is desired, a future narrowly scoped task should
evaluate request/network scheduling first, while separately measuring the
frame-stall behavior; no such change is implemented here.

Original `sqlite-migration/tasks/34-retire-legacy-catalog-cache-sync-persistence.md`
remains **PAUSED**. Compatibility artifacts under `cache/offline/` were
preserved; Task 34 was not started or partially performed.
