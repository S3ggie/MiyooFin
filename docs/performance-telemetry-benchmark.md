# Performance telemetry benchmark result template

Status: **NOT RUN** — no Miyoo Mini Plus benchmark result is claimed by this
repository document.

## Environment

| Field | Value |
|---|---|
| Date/time | NOT RUN |
| Device model / revision | NOT RUN |
| OnionOS version | NOT RUN |
| MiyooFin commit | NOT RUN |
| `PERF_TELEMETRY` build variant | A/B/C: NOT RUN |
| Storage free before run | NOT RUN |
| Network/server condition | NOT RUN |
| Display/audio/power conditions | NOT RUN |
| Scenario content and duration | NOT RUN |
| Laptop OS/Python version | NOT RUN |

## Scenarios and repetitions

Use the commands in [the workflow](performance-telemetry.md). A, B, and C must
use the normal `./launch.sh` OnionOS launcher. B and C use the same compiled-in
binary; C changes only `MIYOOFIN_TELEMETRY=1` at launch.

| Scenario | Meaning | Repetitions | Valid runs | Median duration | Spread | Notes |
|---|---|---:|---:|---:|---:|---|
| A | telemetry compiled out | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| B | compiled in, runtime off | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| C | compiled in, runtime on | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |

## A/B/C comparison

| Metric | A median | B median | C median | B−A delta | C−B delta | C−B % |
|---|---:|---:|---:|---:|---:|---:|
| One-core CPU percent | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| RSS / peak RSS KiB | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Frame maximum / stalls | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Request latency | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Artwork decode latency | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Download bytes / retries | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| Telemetry dropped records | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN | NOT RUN |

For machine-calculated medians, spreads, B−A compile/variant deltas, and C−B
runtime observer-effect deltas, use:

```sh
python3 tools/telemetry/compare_runs.py \
  --a a-run-1-summary.json a-run-2-summary.json \
  --b b-run-1-summary.json b-run-2-summary.json \
  --c c-run-1-summary.json c-run-2-summary.json \
  --json comparison.json
```

## Low-storage and rotation results

| Check | Result | Evidence |
|---|---|---|
| Startup below cutoff creates no trace | NOT RUN | NOT RUN |
| Runtime crossing below cutoff disables tracing | NOT RUN | NOT RUN |
| 16 MiB rotation | NOT RUN | NOT RUN |
| Four-file retention | NOT RUN | NOT RUN |

## Privacy review

| Check | Result | Evidence |
|---|---|---|
| Raw MFT contains no fake secrets/URLs/IDs | NOT RUN | NOT RUN |
| No authenticated URL or HTTP body in copied traces | NOT RUN | NOT RUN |
| Copied traces handled and removed | NOT RUN | NOT RUN |

## Run notes

Record thermal, power, network, server, UI, and test interruptions here. Do not
replace `NOT RUN` with `PASS` without actual device evidence and a reviewer.
