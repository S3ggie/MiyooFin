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
  main.cpp, app/                 Application, ScreenStack, diagnostics
  input/                         SDL input polling and logical Actions
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
    HomeScreenSync.cpp           Library synchronization worker
    HomeScreenRefresh.cpp        Lightweight refresh work
    EpisodeBrowserScreen.cpp     EpisodeBrowserScreen lifecycle/core
    EpisodeBrowserRender.cpp     Episode rendering
    EpisodeBrowserArtwork.cpp    Artwork and bounded prefetch worker
    EpisodeBrowserPlayback.cpp   Playback handoff
    EpisodeBrowserDownloads.cpp  Download actions and planning
  net/
    JellyfinApi.cpp              JellyfinApi core/coordinator
    JellyfinApiJson.cpp          JSON parsing and media conversion
    JellyfinApiAuth.cpp          Authentication and system endpoints
    JellyfinApiLibrary.cpp       Library endpoints
    JellyfinApiHierarchy.cpp     Series/season/episode endpoints
    JellyfinApiPlayback.cpp      Playback and reporting endpoints
    JellyfinApiDownload.cpp      Download/HLS endpoints
  cache/                         Image, library, sync, and offline caches
  download/
    DownloadManager.cpp          DownloadManager lifecycle/core state
    DownloadManagerPlanning.cpp  Planning worker and plan snapshots
    DownloadManagerReconcileWorker.cpp  Reconcile request/worker
    DownloadManagerTransfer.cpp  HLS transfer worker and segment progress
    DownloadReconcile.*          Reconcile policy
  playback/                      Playback request and offline journal
include/miyoofin/                 Public identity/version headers
tests/test_main.cpp               Single test harness and dispatch
tests/cases/*.inc                 Included test groups in one translation unit
assets/, distributions/, docs/    Runtime assets, OnionOS packaging, documentation
output/                           Gitignored build artifacts
```

## Responsibility boundaries

`HomeScreen` is the lifecycle and coordination point. Navigation, rendering, settings, downloads,
artwork decoding, hierarchy/poster work, library synchronization, and lightweight refresh workers
are implemented in concern-specific translation units. Small Home projections and planning helpers
are independent modules that can be tested without the screen.

`EpisodeBrowserScreen` keeps its navigation and lifecycle coordination while rendering, thumbnail
prefetch, playback handoff, and download planning/actions live in separate translation units.

`JellyfinApi` remains the stable API surface. JSON conversion and endpoint families are separated
into JSON, authentication/system, library, hierarchy, playback/reporting, and download/HLS
translation units. Endpoint semantics and request formats remain centralized behind the same API.

`DownloadManager` retains queue, synchronization, and durable-store ownership in its core. Planning,
reconciliation, and HLS transfer/progress work are separated into their own implementation units.
This preserves segmented resumable downloads, `.part` recovery, retries, pause/resume/retry/delete,
and download reconciliation.

The test harness stays in `tests/test_main.cpp`, while related cases are grouped in included
`tests/cases/*.inc` files. They remain one translation unit so shared fixtures, helpers, and test
behavior stay compatible.

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
       └─ screen classes and their concern-specific implementation units
            ├─ HomeScreen + Home models and HomeScreen units
            ├─ EpisodeBrowserScreen + rendering/artwork/playback/download units
            ├─ JellyfinApi + JSON/auth/library/hierarchy/playback/download units
            └─ DownloadManager + planning/reconcile/transfer units
```
