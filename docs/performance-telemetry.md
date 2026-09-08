# Performance telemetry workflow

This document describes the supported desktop and Miyoo Mini Plus workflow for
collecting and interpreting MiyooFin Performance Telemetry. It does not contain
hardware results; the benchmark result template remains `NOT RUN` until a real
device run is performed.

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
