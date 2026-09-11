# SQLite migration hardware evidence

## Task 18 — fresh bootstrap and recovery

Tested on a physical Miyoo Mini Plus running OnionOS, from the package built
from the `f0c96ae87a818dd284ef587996d95205d02c62bf` base plus the Task 18
lifecycle-diagnostic changes in this commit. The deployed ARM binary SHA-256
was `6a95842a82f0c874009faf4d4bebbf8b2a8a63fea969ecc697f7df98d1f4222e`.

The tested scope was the anonymized scope `907a3789cc02e9e4`. Before fresh
bootstrap, no final SQLite database was present. The legacy `catalog.v1` was
recorded only as a rollback artifact and remained unchanged at 7,825,981 bytes
with SHA-256
`2e461899c1b9d44fe366759c7b47c315381747de59bbf84964496f3509c3ca74`.

The persistent CatalogDb diagnostic log proved the transient bootstrap
lifecycle without filesystem polling:

```text
[CatalogDb] bootstrap_temp_open_started
[CatalogDb] bootstrap_temp_created
[CatalogDb] bootstrap_temp_finalized final_present=1
[CatalogDb] final_state ready=1 scope_status=ready(2) error_category=none(0) open_state=created_v1(1)
```

The events occurred in that order at approximately 262230 ms, 262230 ms,
262340 ms, and 262340 ms respectively. After close, only the final
`catalog.sqlite3` remained; no `.migrating`, journal, WAL, or SHM sidecar
remained. A separate finalization-failure event is emitted with only numeric
`errno` if the atomic rename fails.

### Fresh offline bootstrap

With Wi-Fi disabled, the fresh database reconstructed the minimum downloaded
hierarchy from DownloadStore metadata:

| kind | rows |
|---|---:|
| series | 2 |
| seasons | 3 |
| episodes | 29 |
| total | 34 |

All series remained synchronization-incomplete and the committed generation
remained 0. `PRAGMA quick_check` returned `ok`; `PRAGMA foreign_key_check`
returned zero violations. Downloaded content remained browsable and locally
playable during the offline check.

### Fresh online bootstrap and reconciliation

With Wi-Fi available, the fresh database grew beyond the 34-row offline-only
state to 2,338 rows:

| kind | rows |
|---|---:|
| series | 27 |
| seasons | 138 |
| episodes | 2,173 |
| total | 2,338 |

The resulting checkpoint state was 26 complete series and 1 incomplete series;
the committed generation remained 0 until the required complete reconciliation
criteria were met. `PRAGMA quick_check` returned `ok`; `PRAGMA foreign_key_check`
returned zero violations.

### Restart and process recovery

- Normal close and relaunch reopened the existing scoped database without
  rebuilding it.
- The required process-kill scenario left the database present and valid;
  restart preserved the last committed rows and conservative checkpoint state.
- Post-recovery `quick_check` returned `ok`, and `foreign_key_check` returned
  zero violations.
- The legacy catalog remained byte-for-byte unchanged throughout all SQLite
  bootstrap and recovery operations.

### Telemetry and device evidence

The benchmark telemetry run used the supported invocation
`MIYOOFIN_TELEMETRY=1 ./launch.sh` with the normal packaged launcher. The MFT
v2 trace recorded CatalogDb activity with 7 transactions, 6 commits, 525 rows
inserted, 1 row updated, and 1 row deleted. It recorded zero dropped records,
zero writer errors, zero enqueue rejections, and zero SQLite busy, I/O, or
corruption errors; queue high-water was 1.

The device reported ARMv7 hardware, Linux 4.9.84, and approximately 51.1 GiB
free on the 119.1 GiB SD card at capture time. No private media identifiers,
titles, usernames, tokens, SQL text, or authenticated URLs were recorded.

## Task 20 — Miyoo journal/synchronous matrix and decision

The Task 20 matrix ran on the same physical Miyoo Mini Plus and SD card on
2026-09-10 from the Task 19 commit
`918384120bb958af31a5e36a372b65d2c2cdbe80`. The device reported `armv7l`,
Linux `4.9.84`, and 51.0 GiB free on the 119.1 GiB SD card. The ARM benchmark
binary ran from an isolated benchmark directory and did not access application,
download, or user-data paths. Each profile used five repetitions of W1, W2,
W3, W4-1, W4-5, W4-20, W5, W6, and W7, plus mandatory process-interruption
recovery. All 300 workload records and all 30 recovery records returned
`result_code=0`; every recovery record returned `quick_check=ok` and zero
foreign-key violations.

The exact candidate configurations were:

| profile | journal | synchronous | locking |
|---|---|---|---|
| A | DELETE | FULL | NORMAL |
| B | DELETE | EXTRA | NORMAL |
| C | WAL | FULL | NORMAL |
| D | WAL | NORMAL | NORMAL |
| E | WAL | FULL | EXCLUSIVE |
| F | WAL | NORMAL | EXCLUSIVE |

The following are the median transaction duration plus the separately measured
WAL checkpoint duration in microseconds for each workload. DELETE profiles
have no checkpoint phase:

| profile | W1 | W2 | W3 | W4-1 | W4-5 | W4-20 | W5 | W6 | W7 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| A | 22286+0 | 25853+0 | 241564+0 | 26384+0 | 69978+0 | 212551+0 | 51953+0 | 23288+0 | 39513+0 |
| B | 22195+0 | 30565+0 | 247427+0 | 28581+0 | 71082+0 | 208428+0 | 58059+0 | 24261+0 | 47425+0 |
| C | 11168+10295 | 17519+8844 | 226407+50323 | 16989+11284 | 57576+19043 | 203670+46671 | 33884+8961 | 17805+16444 | 56027+7837 |
| D | 5107+18430 | 9006+22890 | 190805+94542 | 9095+19390 | 41686+36171 | 157191+84387 | 21348+22797 | 17071+30486 | 50198+15877 |
| E | 10981+7112 | 14129+11573 | 228661+50257 | 15718+8238 | 54597+19460 | 194638+45598 | 34364+8114 | 16841+16339 | 52297+7916 |
| F | 5524+18681 | 8802+20076 | 189095+96445 | 9073+18300 | 41338+37514 | 156978+82674 | 25261+22786 | 15257+29943 | 46769+16788 |

The checkpoint-inclusive mean of the nine workload medians was 79,263 us for
A, 82,003 us for B, 91,092 us for C, 94,649 us for D, 90,201 us for E, and
93,563 us for F. WAL profiles reduced transaction-only time in several
workloads, but their required checkpoint phase made the end-to-end result
mixed and did not beat the durable baseline overall. B was also slower overall
than A. The benchmark therefore retains A (`DELETE` + `FULL` + `NORMAL`) as
the conservative production profile; no application profile change was
justified before CP-D.

Across workload records, kernel-reported process read/write bytes were zero on
this device and are recorded as zero/unavailable rather than inferred SD I/O.
Median RSS was 3,880 KiB for A, 3,904 KiB for B/C, and 3,896 KiB for D/E/F. A
representative final database size was 57,344 bytes for DELETE and 4,096 bytes
in the main file with a 140,112-byte WAL for WAL profiles at the sampling
point. WAL recovery samples retained a 32,768-byte SHM sidecar; the EXCLUSIVE
ordinary workload samples had no SHM sidecar. The ARM runtime printed a
shared-library version-information warning, but all records still completed
with result code 0.

The mandatory process-interruption test used an active transaction for every
profile. Recovery completed successfully for A–F, with no corruption, no
foreign-key violations, and no committed rows from the interrupted
transaction. No controlled hard-power test was performed because explicit
human approval for that destructive test was not provided.

An ordinary physical menu launch using the retained A production profile
reopened the expected scope and completed normally. Persistent numeric
diagnostics recorded CatalogDb startup, scoped open, and hierarchy activity;
the UI-stall log recorded a 3,519 ms startup stall including Home screen
construction, a 444 ms Home update, and a 611 ms later interaction stall. The
menu launch did not inherit `MIYOOFIN_TELEMETRY=1`, so no new MFT trace is
claimed for that run. Existing supported MFT v2 evidence from Task 18 remains
the source for CatalogDb counters and zero-drop/error confirmation. Because A
was both the baseline and the evidence-backed retained candidate, no separate
finalist production profile was deployed or selected for an app-level A/B run.
No WAL or reduced-synchronous setting was enabled in the application.

## Task 27 — controlled hierarchy A/B evidence (CP-F)

Task 27 was rerun as a controlled persistence benchmark on the same physical
Miyoo Mini Plus and Jellyfin server. The invalid earlier SQLite run with zero
ChangedHierarchy items is excluded. The control was
`b9efee216ad40e17b0c787c959a3a91fd8748d5a` plus the benchmark-only graceful
exit hook; it is not a byte-identical historical binary. The SQLite build was
`e9a6611b3ae981be738a8846896a43493bfff6b5` plus the already-validated remote
exit tooling at the benchmark HEAD. The A′ hook patch SHA-256 was
`67e64bde2c04473599c52b0ceaf89780c6e876d0158946aa405263aa48168a27`.

Before each run, the complete scoped state was restored from the same tar
backup (SHA-256
`24e295a9dccad4467e517695bc17f470f9a05dac4c5aaf342c67711fc9c37239`). The
legacy and SQLite checkpoints were set to the same timestamp approximately 23
hours old, with a fresh reconcile timestamp, keeping the run inside the
incremental-sync window. Downloads and authentication state were not touched.
Each run used the Onion-native telemetry launcher, waited for ChangedHierarchy
and HomeHierarchy completion, then used only the graceful SIGUSR1 exit helper.

All four measured runs were workload-equivalent: 228 ChangedHierarchy items,
7 ChangedHierarchy requests, 1,407 media items, 21 HomeHierarchy completions,
zero HomeHierarchy failures/cancellations, and queue high-water 21. The
objective completion condition was reached before exit; no unrelated manual
navigation was used.

| metric | A′ rep 1 | A′ rep 2 | B rep 1 | B rep 2 |
|---|---:|---:|---:|---:|
| LibrarySync | 37.485 s | 37.785 s | 33.836 s | 35.000 s |
| all network requests / payload | 204 / 8,405,608 B | same | same | same |
| ChangedHierarchy latency | 3.766 s | 3.881 s | 3.975 s | 3.688 s |
| HomeHierarchy completed/failed/cancelled | 21/0/0 | 21/0/0 | 21/0/0 | 21/0/0 |
| queue HWM | 21 | 21 | 21 | 21 |
| CPU average / p95 / max (2-core device %) | 45.7/60.8/75.0 | 30.5/50.9/75.4 | 45.2/64.2/78.3 | 48.9/64.8/79.4 |
| RSS average / sampled max / process peak (KiB) | 47,821/75,076/81,248 | 50,764/79,468/80,620 | 27,055/29,260/29,260 | 25,665/28,000/28,000 |
| telemetry drops / writer errors | 0/0 | 0/0 | 0/0 | 0/0 |

## Task 33 — final Home/library validation (CP-G)

The final Task 33 validation used the ARM package produced from commit
`4737d5832d511e331715dbe3da9ce0706a680273` on the same physical Miyoo Mini
Plus, through the centralized Onion-native launcher on SSH port 2222. The
package passed `make onionos verify-arm` before deployment. The existing Task
32/Task 27 hardware runs already established bounded Home paging, Movies and
Shows navigation, Series/EpisodeBrowser navigation, media open/back flows,
offline/download/local playback behavior, stale-work suppression, and the
required schema-v3 integrity checks. No production code was changed for this
benchmark task.

The final current-build lifecycle run launched with telemetry enabled, reached
the Onion-native foreground state, and was terminated with the validated
graceful SIGUSR1 helper while background activity was active. MiyooFin exited,
MainUI was restored, and no MiyooFin process remained. The refreshed MFT trace
was analyzed with `--cpu-count 2`:

| metric | final current-build run |
|---|---:|
| telemetry records | 29 health records |
| telemetry drops / writer errors | 0 / 0 |
| telemetry queue high-water | 3 |
| frame samples | 3,189 |
| frame stalls >50 ms / >100 ms | 726 / 5 |
| maximum frame duration | 100,601 µs |
| CatalogDb artifact | schema-v3 `catalog.sqlite3`, 21,028,864 B |
| MainUI restoration | PASS |
| graceful exit with active background work | PASS |

The >50 ms and >100 ms frame observations are attributable to the known
framebuffer-upload boundary on this hardware; they do not show a new SQLite or
UI-thread regression. The device image does not include a sqlite3 CLI, so the
final integrity values remain the previously recorded hardware evidence:
`PRAGMA quick_check = ok`, zero `foreign_key_check` rows, and schema version 3.

Paired means, with SQLite relative to A′: LibrarySync was 34.418 s versus
37.635 s (**−8.5%**); ChangedHierarchy request latency was 3.831 s versus
3.823 s (**+0.2%**); CPU average was 47.1% versus 38.1% (**+23.6%**, with
wide run-to-run variation); sampled RSS was 26,360 KiB versus 49,292 KiB
(**−46.5%**), and process peak RSS was 28,630 KiB versus 80,934 KiB
(**−64.6%**). Payload and request counts were identical, so network volume is
not an explanation for the LibrarySync difference.

SQLite recorded 23 transactions, 22 commits, 2,346 inserted rows, 253 updated
rows, and 119 deleted rows in each B run. Commit maximums were 6.660 s and
6.943 s (mean 6.801 s); the current summary telemetry does not expose a
per-commit sample series, so a distinct commit p95 cannot be calculated from
these traces. Transaction maximums were 10.314 s and 10.468 s (mean 10.391 s).
No SQLite busy, I/O, corruption, or foreign-key errors occurred; post-run
`quick_check` was `ok` and foreign-key checks were empty. The A′ architecture
has no CatalogDb transaction series because it persists hierarchy through
OfflineCatalog.

Frame pacing was interpreted using the target paced frame interval rather than
the `>50 ms` bucket, which is near the expected framebuffer-upload boundary on
this hardware. FullFrame `>100 ms` counts were A′ 27 and 19, versus B 28 and
35. The complete FullFrame histograms (buckets in the telemetry schema) were
A′ `[15965,12,3,0,1780,4592,23,2,2]` and
`[130654,17,0,1,7822,44439,11,2,6]`; B
`[12688,9,1,0,577,4474,28,0,0]` and
`[10061,13,0,0,484,3512,35,0,0]`. Input, Update, ScreenRender, and
FramebufferUpload timings were separately retained in the decoded traces;
the dominant long-duration phase was framebuffer upload, while Update and
Input had only isolated >100 ms samples. UiStall records and phase summaries
were present, with no telemetry loss. These measurements do not establish a
frame-timing regression from the `>50 ms` count.

The remaining `OfflineCatalog` references previously classified in
`HomeScreenSync` and `EpisodeBrowserArtwork` were not active catalog
load/save paths: the former is a compatibility/offline projection object and
the latter is an unused include. No runtime legacy load/save access was
observed or introduced by this benchmark.

Both architectures exited cleanly twice through the remote SIGUSR1 path;
MainUI returned after every run, no queue residue remained, telemetry traces
decoded successfully, and the SQLite catalog remained valid. The temporary
control worktree was removed after evidence collection. No production code or
benchmark workload was changed for the comparison.
