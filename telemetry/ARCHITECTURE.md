# Canonical Performance Telemetry Architecture

Baseline: `main` @ `ef79ba85229f22d270fcf57072823050c371bf33`.

## Current owners

`src/main.cpp` owns process lifetime; `src/app/App.cpp` owns SDL/update/render/present and playback handoff; `UiDiagnostics` owns UI stall detection; ScreenStack owns deferred retirement; HomeScreen split files own sync/hierarchy/poster/decode/refresh workers; EpisodeBrowser owns fetch/artwork workers; ImageCache owns cache I/O; ImageDecoder owns JPEG decode; JellyfinApi owns semantics; RouteRequest owns route attempts; HttpClient owns normal libcurl; DownloadManager owns transfer/planning/reconcile and direct HLS.

## Lifecycle

```text
main
  config = TelemetryConfig::fromEnvironment()
  PerformanceTelemetry::start(config)
  {
    App app
    App::init()
    App::run() only if init succeeded
    App destructor
  }
  PerformanceTelemetry::stop()
  return result
```

Telemetry startup failure never fails the app. App-init failure still destroys App before telemetry stop.

## Disabled paths

Compile-out excludes telemetry production `.cpp` files and preprocessor-eliminates hot telemetry work. Runtime-off has no service thread/file and uses a relaxed `enabledFast()` check before any telemetry clock/TLS-context/event work. Non-frame operations use guarded TelemetryTimer/context scopes; App takes one enabled snapshot per frame and calls the telemetry clock only when true.

Current build outputs are shared across flags; always `make clean` before switching `PERF_TELEMETRY` 0↔1.

## Core modules

- `TelemetryIds.hpp`: schema-stable numeric enums.
- `TelemetryTypes.hpp`: fixed POD payloads; no dynamic strings; `TelemetryRecord <=96 bytes`.
- `TelemetryClock.hpp`: `clock_gettime(CLOCK_MONOTONIC)` and `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`.
- `TelemetryContext.hpp`: enum-only thread-local RequestKind/RouteKind/attempt/fallback/ArtworkContext.
- `TelemetryGuards.hpp`: compile/runtime guarded timer/context helpers.
- `TelemetryRing.hpp`: 512-slot bounded ARM-safe MPSC queue.
- `MftFormat.*`: explicit little-endian MFT v1 codec.
- `TelemetryWriter.*`: service-thread-owned 32KiB buffer, <=5s flush cadence, 16MiB rotation, four retained files.
- `LinuxProcessMetrics.*`: process CPU, RSS, peak RSS, `/proc/self/io`, statvfs.
- `PerformanceTelemetry.*`: facade, ring, gauges/counters, frame accumulators, service thread, writer/sampler ownership.

## Service cadence and aggregate ownership

Base tick: 1000ms from monotonic deadlines, no catch-up bursts. Free space refresh: 10000ms.

### SystemSample
Once per base tick. CPU/RSS/peak/read/write are cumulative raw values. Free storage is carried between 10s refreshes with age.

### WorkerSample
One record per defined WorkerId every tick. `active` and `queue_depth` are instantaneous. `queue_highwater`, completed/failed/cancelled are interval values and reset after emission; high-water resets to at least current depth.

Workers only update facade atomics while already in their owner code; service takes no worker mutex.

### ArtworkSummary
Exactly one per tick. **ImageCache alone** feeds cache probe/read/write and compressed-byte counters. **ImageDecoder alone** feeds decode count/failure/total/max. All are interval values and reset after emission. Pipeline tasks only establish context/worker state and must not duplicate these measurements. Individual ArtworkDecode records are emitted only by ImageDecoder.

### DownloadSample
Exactly one per tick. DownloadManager supplies current active/queued/planner depth and interval byte delta; HLS supplies segment-completed/retry deltas. `bytes_per_sec = bytes_delta * 1,000,000 / actual_interval_us`. Delta counters reset after emission.

### TelemetryHealth
Exactly one per tick. Queue depth/buffered bytes instantaneous; queue high-water interval and reset to current; drops/writer errors/rotations/sampling-late cumulative.

## Measurement ownership — no double counting

### Worker instrumentation owns
Activity/idle state, queue depth/high-water, completion/failure/cancellation, and semantic context establishment only.

### ImageCache owns
Generic cache probe hit/miss, read/write success/failure, and compressed cache bytes. Pipeline tasks must not duplicate these counters.

### ImageDecoder owns
Decode duration, compressed input bytes, decoded RGBA bytes, decode aggregate counters, and the individual ArtworkDecode record. Pipeline tasks must not emit a second decode measurement.

### JellyfinApi / RouteRequest / HttpClient ownership
JellyfinApi owns RequestKind context; RouteRequest owns route/attempt/fallback context; HttpClient owns normal/binary transport measurements. Direct HLS transport remains owned by DownloadManagerTransfer.

## Networking ownership

```text
JellyfinApi: RequestKind only
  ↓
RouteRequest: RouteKind + attempt + fallback only
  ↓
HttpClient: actual duration/status/CURLcode/payload record
```

Direct artwork callers set RequestKind::Artwork. HLS direct libcurl attempts are measured only in DownloadManagerTransfer, not HttpClient.

## UiDiagnostics

Remains authoritative for heartbeat, 500ms stall, 100ms slow scope, human log, and recent text. Bridge maps only allowlisted numeric phase/screen/tab/action/scope plus worker mask. No formatted watchdog line enters MFT.

## Low-storage safety

Default `minFreeStorageBytes = 128 MiB`. Optional `MIYOOFIN_TELEMETRY_MIN_FREE_MIB` is numeric and clamped 64–4096 MiB.

Before first trace open, service samples free space. If valid and below threshold: create no trace, emit one safe stderr notice, set runtime enabled false, exit service. During tracing, check on the normal 10s cadence. On first valid crossing below threshold: directly append WriterDisabledLowSpace SessionEvent, then final TelemetryHealth, flush/close, set runtime enabled=false, stop further sampling/writing, and exit service. Never auto-reenable in that process and never delete app/download data.

## Playback

Measure request→terminal present, suspend, child wait, resume, return→first normal frame. Suspend periodic sampling during child wait and reset deadlines on resume. Preserve App's existing `m_lastTick = SDL_GetTicks()` reset.

Hardware runs must use the normal Onion launcher: `MIYOOFIN_TELEMETRY=1 ./launch.sh`.
