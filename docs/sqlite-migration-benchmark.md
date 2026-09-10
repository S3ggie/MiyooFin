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
