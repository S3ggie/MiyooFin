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
