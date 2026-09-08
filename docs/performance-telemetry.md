# Performance telemetry workflow

This document describes the supported desktop and Miyoo Mini Plus workflow for
collecting and interpreting MiyooFin Performance Telemetry. The completed
real-device results are recorded in the
[performance telemetry benchmark](performance-telemetry-benchmark.md).

MFT v1 is frozen. The normative binary contract is
[`telemetry/SCHEMA_V1.md`](../telemetry/SCHEMA_V1.md), the implementation is
described in [`docs/architecture.md`](architecture.md), and the laptop tools
consume that contract independently.

## Build and runtime variants

The telemetry production code is selected at compile time by `PERF_TELEMETRY`.
The normal host build and tests use the default compiled-in value:

```sh
make clean
make PERF_TELEMETRY=1 -j2
make test -j2
```

For a compile-out comparison, clean before changing the variant, then build and
test with `PERF_TELEMETRY=0`:

```sh
make clean
make PERF_TELEMETRY=0 -j2
make test PERF_TELEMETRY=0 -j2
```

Always run `make clean` before changing `PERF_TELEMETRY=0` to `1` or `1` to `0`.
The build outputs are shared across flags. Build A and B/C from the same source
revision; B and C must use the same compiled-in binary, with only the runtime
environment switch changed.

The benchmark evidence passed the required non-soak scenarios on the Miyoo Mini
Plus, including a valid growing C trace, normal network and local playback
lifecycles, and isolated low-storage shutdown. The user-waived 1–2 hour soak was
not performed. Numeric CPU/RSS/frame observer-effect medians were not captured
and are not inferred as passes; see the benchmark report for the exact evidence
and remaining limitations.

## Normal OnionOS launch scenarios

Copy the packaged application to the device as usual. Run each scenario through
the normal OnionOS launcher from the packaged application directory:

```sh
# A: telemetry compiled out / baseline
./launch.sh

# B: telemetry compiled in, runtime off
MIYOOFIN_TELEMETRY=0 ./launch.sh

# C: telemetry compiled in, runtime on
MIYOOFIN_TELEMETRY=1 ./launch.sh
```

Do not launch the application binary directly for a benchmark. The launcher sets
the OnionOS SDL and shared-library environment needed for a representative run.
Use the same device state, content, brightness, network, and scenario for all
three cases. Repeat each case enough times for medians and spread; record every
interruption or environmental deviation.

## Trace copy and desktop analysis

When C is runtime-enabled, traces are written under:

```text
/mnt/SDCARD/App/MiyooFin/telemetry-logs/
```

Copy the retained `.mft` files off the device before analysis. On the laptop:

```sh
python3 tools/telemetry/decode.py trace.mft --json trace.json --csv-dir trace-csv
python3 tools/telemetry/analyze.py trace.mft --json trace-summary.json --csv-dir trace-analysis
```

The decoder and analyzer use only the Python standard library. Analysis derives
CPU and I/O rates from monotonic deltas and keeps the device producer path numeric
and bounded. `--cpu-count N` additionally reports whole-device CPU share.

## Benchmark acceptance checks

Keep device, filesystem, OnionOS/firmware, Wi-Fi, Jellyfin path and media,
application settings, display conditions, and power/clock condition constant
across variants. Disable unrelated device activity. Use repeated short runs when
practical and report interruptions and environmental deviations; the completed
results are linked above.

The comparison checks compile-out removal, runtime-off absence of telemetry thread
and file, guarded runtime-off clocks/context/event work, no added baseline UI
stalls, ordinary trace rate in the few-KiB/s range, zero ordinary drops, bounded
rotation, and low-storage shutdown. CPU/RSS/frame thresholds are reported only
when measured; they are never inferred from functional behavior. A low-storage
crossing must include `WriterDisabledLowSpace`, final health, writer close, the
disabled state, no further trace growth, and unaffected application/download
behavior. The main application filesystem must not be filled to cross the cutoff.

## Final coverage and ownership review

The frozen implementation has one owner for each telemetry domain:

| Domain | Owner / guarantee |
|---|---|
| Process, cadence, aggregates, health | `PerformanceTelemetry` service thread; interval counters reset after emission and gauges remain instantaneous. |
| Frames, UI state, and playback handoff | `App` and `ScreenStack`; five playback stages are recorded and sampling is suspended only during external child playback. |
| Watchdog diagnostics | `UiDiagnostics`; it remains a sibling authority and only allowlisted numeric mappings enter MFT. |
| Workers and screen retirement | Each owning worker updates its facade state; the service thread emits worker snapshots without taking worker mutexes. |
| Artwork | `ImageCache` owns generic cache aggregates; `ImageDecoder` owns decode aggregates and individual decode records. |
| Jellyfin and normal transport | `JellyfinApi`, `RouteRequest`, and `HttpClient` own semantic request, route-attempt, and transport records respectively. |
| Downloads and HLS | `DownloadManager` owns download state and direct HLS attempt/retry records; no normal transport double-counting occurs. |
| Desktop analysis | `tools/telemetry` decodes MFT v1 and derives summaries without changing device records. |

Compile-out removes production telemetry and hot-path work. Runtime-off avoids
the service thread, trace file, telemetry clocks, TLS context mutation, and event
work. The writer samples the configured output filesystem, refuses startup below
128 MiB, and disables tracing on the first valid runtime crossing without deleting
application or download data.

MFT v1 is allowlist-only: it serializes no strings, credentials, authenticated
URLs, headers, bodies, persistent Jellyfin identifiers, titles, image tags, cache
paths, or download scopes. The complete hardware, privacy, low-storage, and
observer-effect review is linked from the benchmark report above.

Allowed trace data is limited to fixed enums, booleans, counts, durations, byte
counts, queue depths, numeric HTTP/CURL codes, build metadata, and ephemeral
process-local sequence IDs/nonces. Persistent identifiers must not be hashed as a
workaround.

## Safety and privacy

Telemetry is disabled at runtime with `MIYOOFIN_TELEMETRY=0`; compile-out removes
the production telemetry implementation. The writer refuses to start when valid
free storage is below the configured 128 MiB default cutoff. The cutoff may be
changed only through the numeric `MIYOOFIN_TELEMETRY_MIN_FREE_MIB` setting, clamped
by the implementation to 64–4096 MiB.

MFT v1 is fixed-layout and allowlist-only. It contains no strings, authenticated
URLs, tokens, headers, bodies, credentials, persistent IDs, titles, image tags,
cache paths, or download scopes. The writer rotates at 16 MiB and retains four
files by default. Remove copied traces after analysis according to the local
handling policy.
