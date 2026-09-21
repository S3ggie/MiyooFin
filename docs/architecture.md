# MiyooFin Architecture

## Overview

MiyooFin is a C++17 SDL2 application for the Miyoo Mini Plus running OnionOS. It uses a single
software framebuffer: drawing targets one 640x480 RGBA32 surface, which is uploaded to a streaming
texture and presented each frame.

## Rendering, screens, and input

- One `SDL_Surface` (640x480, RGBA32) is the software framebuffer.
- One `SDL_Texture` (STREAMING) is uploaded from the surface each frame.
- The UI does not use `SDL_TEXTUREACCESS_TARGET`, which is unavailable on the Miyoo SDL2 build.
- UI elements do not use `SDL_CreateTextureFromSurface`.
- The embedded 8x16 bitmap font avoids an SDL2_ttf dependency.
- `Screen` defines `enter`, `leave`, `handleAction`, `update`, and `render`; `ScreenStack` manages
  navigation and only the top screen receives events and renders.
- `InputManager` polls SDL events and converts them to logical `Action` values. Raw events are
  retained for the diagnostics screen.

## Source organization

The public screen, API, and download classes remain stable interfaces. Their implementations are
organized by responsibility so lifecycle and coordination code stays small without changing the
existing behavior or ownership model.

```
src/
  main.cpp, app/                 Application, AppSession/AppPlayback lifecycle units, ScreenStack
  input/                         SDL input polling and logical Actions
  data/                          MediaItem and title/catalog ordering value helpers
    MediaItem.hpp                Canonical media item value type
    CatalogPrimitives.*          Organization/sort/alphabet primitives
    TitleOrganization.hpp        Value-level title ordering facade
    MovieTitle.hpp               Movie organization rules
  diagnostics/                   Performance telemetry and UI stall diagnostics
    UiDiagnostics.*              Watchdog heartbeat, stall edges, slow scopes
  ui/                            Shared UI, font, and Home pure-logic models
    HomeSyncState.hpp            Sync-state projection helpers
    HomeSettingsModel.*          Settings presentation model
    HomeTabs.*                   Home tab projection
    HomeArtworkPlan.*            Artwork planning
  ui/screens/
    HomeScreen.cpp               HomeScreen lifecycle/coordinator core
    HomeScreenNavigation.cpp     Home navigation and input
    HomeScreenRender.cpp         Home rendering
    HomeScreenDownloads.cpp      Downloads-tab behavior
    HomeScreenSettings.cpp       Settings-tab behavior
    HomeScreenArtwork.cpp        Artwork/decode worker work
    HomeScreenHierarchy.cpp      Series/season hierarchy work
    HomeScreenSync.cpp           Coordinator publication consumption and Home sync state
    HomeScreenOffline.cpp        Offline projection preparation/application
    HomeScreenSyncApply.cpp      UI-thread sync result publication
    HomeScreenRefresh.cpp        Lightweight refresh work
    EpisodeBrowserScreen.cpp     EpisodeBrowserScreen lifecycle/core
    EpisodeBrowserRender.cpp     Episode rendering
    EpisodeBrowserArtwork.cpp    Artwork and bounded prefetch worker
    EpisodeBrowserPlayback.cpp   Playback handoff
    EpisodeBrowserDownloads.cpp  Download actions and planning
    EpisodeBrowserData.cpp       Episode cache/network data preparation
    SeriesScreen.cpp             Series lifecycle/update coordinator
    SeriesScreenWorker.cpp       Series refresh/artwork workers
    SeriesScreenNavigation.cpp   Series input and selection navigation
    SeriesScreenRender.cpp       Series layout and rendering
    MovieDetailsScreen.cpp       Movie lifecycle/input coordinator
    MovieDetailsWorker.cpp       Movie artwork/data worker
    MovieDetailsRender.cpp       Movie layout and rendering
  catalog/
    CatalogDb.cpp                Catalog worker/queue/lifecycle coordination
    CatalogDbSchema.cpp          Schema/bootstrap/migration implementation
    CatalogDbWrite.cpp           Catalog mutations and transactions
    CatalogDbQuery.cpp           Bounded reads and row decoding
    CatalogDbSyncState.cpp       Durable sync checkpoint state
    CatalogDbHierarchy.cpp       Hierarchy persistence and reconciliation
    CatalogDbTestCommands.cpp    Test-only command queue/dispatch (MIYOOFIN_TEST_BUILD)
  library/
    LibraryCoordinator.*         Session-scoped sync/query lifecycle and publication
    LibraryChangeTypes.hpp       Domain library-change publication types
    LibraryQuery.*               Domain-neutral bounded catalog reads and result types
    LibrarySync.cpp              Lower-level sync primitive used by the coordinator
    LibrarySyncIncremental.cpp  Incremental and authoritative sync operations
    LibrarySyncEvents.cpp        Live event queue plumbing for the coordinator
  net/
    JellyfinApi.cpp              JellyfinApi core/coordinator
    JellyfinApiJson.cpp          JSON parsing and media conversion
    JellyfinApiAuth.cpp          Authentication and system endpoints
    JellyfinApiLibrary.cpp       Library endpoints
    JellyfinApiHierarchy.cpp     Series/season/episode endpoints
    JellyfinApiPlayback.cpp      Playback and reporting endpoints
    JellyfinApiDownload.cpp      Download/HLS endpoints
    HlsPlaylist.*                HLS playlist parsing and URL resolution
    HlsProfile.hpp               Constrained device transcode profile/estimates
  cache/                         Image, library, and sync caches
  download/
    DownloadManager.cpp          DownloadManager lifecycle/core state
    DownloadManagerPlanning.cpp  Planning worker and plan snapshots
    DownloadManagerReconcileWorker.cpp  Reconcile request/worker
    DownloadManagerTransfer.cpp  HLS transfer worker and segment progress
    DownloadReconcile.*          Reconcile policy
  playback/                      Playback request, offline journal, offline library projection
include/miyoofin/                 Public identity/version headers
tests/test_*.cpp                  Focused test-binary wrappers and aggregate runner
tests/cases/*.inc                 Test cases shared by focused wrappers
assets/, distributions/, docs/    Runtime assets, OnionOS packaging, documentation
output/                           Gitignored build artifacts
```

## Responsibility boundaries

`LibraryCoordinator` is the singular production owner of the shared library services. It constructs
and owns `LibrarySync` and `LibraryQuery`, serializes synchronization and catalog mutations, and
publishes immutable startup, population, maintenance, hierarchy, live-change, and Home-rail results.
The application creates the coordinator for the active catalog scope and passes its published
boundary/query to consumers. Production UI code does not construct or use raw `LibrarySync`, and
`LibraryQuery` is constructed only by the coordinator.

`HomeScreen` is a presentation consumer and lifecycle point for Home. It submits coordinator
requests and consumes coordinator-published results; it owns navigation, tabs, rows, artwork state,
and offline/Home presentation state. Home projections and planning helpers remain independent
modules that can be tested without the screen. `src/net` converts Jellyfin responses to API/domain
values and does not construct Home models. `MediaItem` remains a domain-neutral value type with no
SDL, UI-framework, or presentation state.

`EpisodeBrowserScreen` keeps its navigation and lifecycle coordination while rendering, thumbnail
prefetch, playback handoff, and download planning/actions live in separate translation units.

`JellyfinApi` remains the stable API surface. JSON conversion and endpoint families are separated
into JSON, authentication/system, library, hierarchy, playback/reporting, and download/HLS
translation units. Endpoint semantics and request formats remain centralized behind the same API.

`DownloadManager` retains queue, synchronization, and durable-store ownership in its core. Planning,
reconciliation, and HLS transfer/progress work are separated into their own implementation units.
This preserves segmented resumable downloads, `.part` recovery, retries, pause/resume/retry/delete,
and download reconciliation.

The test cases remain grouped in `tests/cases/*.inc` files, but focused `tests/test_*.cpp` wrappers
compile them into independent binaries. `tests/test_support.hpp` contains shared fixtures and
assertion support, while `output/test/test_runner` runs every group for the aggregate test target.
Production sources are compiled once into reusable test objects and archived for selective linker
extraction, so changing one case only rebuilds and relinks its focused binary. Desktop-only runtime
checks are exposed through the separate `Makefile.desktop` targets rather than `make test`.

Test-only production seams are separated from the device build. `CatalogDb`'s test command queue,
dispatch, and `*ForTest` definitions live in `catalog/CatalogDbTestCommands.cpp`, which is compiled
only when `MIYOOFIN_TEST_BUILD` is defined and is registered only in the test source list. The
remaining `*ForTest` helpers are small guarded definitions inside their owning modules.

Module includes flow low-to-high: `data/` and `diagnostics/` are leaves; `net/`, `catalog/`,
`cache/`, `download/`, `library/`, and `playback/` build on them; `app/` and `ui/` consume the rest.
`tools/refactor-check.sh` enforces the CatalogDb boundary, the render-only MovieDetails boundary,
test-only source registration, the production UI boundary around `LibrarySync`, and the absence of
stale include paths. Download planning receives the coordinator-exposed `LibraryQuery` and
coordinator services/results; it does not construct or consume raw production `LibrarySync`.
Library synchronization reads the durable `DownloadStore` for offline-catalog reconciliation. These
download/library crossings are deliberate; no other upward crossing is.

The deterministic test boundary uses local catalog/database and loopback fixtures. Focused tests
must not rely on public-network timing or availability to prove synchronization, publication, or
presentation behavior; public-network behavior is covered only by the runtime integration path.

## Performance telemetry architecture

Performance telemetry is a sibling of `UiDiagnostics`. `UiDiagnostics` remains authoritative for
the watchdog heartbeat, stall edges, slow scopes, human-readable diagnostics, and its recent text
history. `PerformanceTelemetry` owns the fixed-layout performance trace and does not replace or
format watchdog output.

The final data path is:

```text
numeric producers
  └─ PerformanceTelemetry facade
       └─ bounded 512-slot MPSC ring
            └─ service thread
                 ├─ LinuxProcessMetrics sampling and interval aggregates
                 ├─ TelemetryWriter buffering, low-storage checks, and rotation
                 │    └─ explicit little-endian MFT v1 records
                 └─ trace files under the configured telemetry output directory
                      └─ laptop decoder / analyzer / comparison tools
```

The facade is the producer boundary. Application, screen, worker, cache, decoder, networking,
download, HLS, and playback code publish only fixed POD records, enum context, or atomic gauges.
Producers do not write files, read process or filesystem metrics, serialize strings, or block for
telemetry. The bounded ring drops under pressure and exposes cumulative drops through health
records. The service thread alone drains the ring, samples process metrics, emits the periodic
aggregates, and owns the writer. `LinuxProcessMetrics` performs `/proc` and `statvfs` work only on
that service thread; storage sampling follows the filesystem containing the configured output
directory.

Process lifetime follows the application lifetime: `main` reads `TelemetryConfig`, starts
`PerformanceTelemetry`, constructs and runs `App`, destroys `App`, then stops telemetry. A telemetry
startup failure never fails the application, and an application-init failure still destroys `App`
before telemetry is stopped. Runtime-off has no service thread or trace file and checks the relaxed
`enabledFast()` flag before telemetry clocks, thread-local context changes, or event work. The
compile-out build removes telemetry production objects and hot-path telemetry work; shared build
outputs are rebuilt cleanly when switching compile variants.

`MftFormat` explicitly encodes the frozen MFT v1 header and records in little-endian order. The
independent standard-library laptop decoder validates the same header, sizes, record layouts, and
allowlisted enum values before analysis. The normative contract is
[`telemetry/SCHEMA_V1.md`](../telemetry/SCHEMA_V1.md).

The proven defaults are a 1000 ms base sample/aggregate cadence, a 10000 ms free-storage refresh,
a 32768-byte writer buffer, 16 MiB rotation, four retained trace files, and a 128 MiB low-storage
cutoff. Runtime configuration may only adjust the documented numeric settings and remains
clamped by the implementation. Real-device observer-effect and low-storage evidence is recorded
in the [performance telemetry benchmark](performance-telemetry-benchmark.md).

Each base tick emits one `SystemSample`, one sample for every defined worker, one
`ArtworkSummary`, one `DownloadSample`, and one `TelemetryHealth`. Worker activity and queue depth
are instantaneous; worker high-water and completion/failure/cancellation values are interval
deltas reset after emission. Artwork cache counters belong only to `ImageCache`, decode counters
and individual decode records belong only to `ImageDecoder`, and download byte/segment deltas are
reset after emission. Download throughput uses the actual elapsed interval. Health drops, writer
errors, rotations, and sampling-late counts are cumulative; queue depth and buffered bytes are
instantaneous. No worker mutex is taken by the service thread.

## Threading, local-first behavior, and persistence

SDL event handling, screen updates, and rendering are nonblocking. HTTP requests, curl transfers,
retry delays, artwork decoding, cache scans, removable-storage work, and potentially blocking worker
joins run on the bounded background-worker architecture. Screen-owned worker state follows the
existing cancellation and lifetime rules.

MiyooFin renders valid cached content immediately and continues offline wherever cached data is
sufficient. Complete validated downloads are preferred for local playback. Session files,
library/image caches, download manifests and indexes, `.part` recovery, playback request/result
files, offline journals, Jellyfin request formats, TLS policy, and canonical public/LAN route
fallback remain behavior-facing boundaries.

## High-level dependency graph

```
main.cpp
  └─ App / ScreenStack / InputManager
       ├─ LibraryCoordinator (LibrarySync + LibraryQuery lifecycle/publication)
       └─ screen classes and their concern-specific implementation units
            ├─ HomeScreen + Home models and HomeScreen units
            ├─ EpisodeBrowserScreen + rendering/artwork/playback/download units
            ├─ JellyfinApi + JSON/auth/library/hierarchy/playback/download + HLS units
            └─ DownloadManager + planning/reconcile/transfer units
  data / diagnostics (leaves; no upward includes)
```
