# MiyooFin Architecture

## Overview

MiyooFin is a C++17 SDL2 application using a single-software-framebuffer
rendering approach. All drawing is done onto one 640x480 RGBA32 surface
which is uploaded to a streaming texture and presented each frame.

## Rendering

- One `SDL_Surface` (640x480, RGBA32) — the software framebuffer
- One `SDL_Texture` (STREAMING) — uploaded from the surface each frame
- No `SDL_TEXTUREACCESS_TARGET` (not supported on Miyoo SDL2)
- No `SDL_CreateTextureFromSurface` for UI elements
- Bitmap font (8x16) embedded as pixel data — no SDL2_ttf dependency

## Screen System

- `Screen` abstract interface: enter/leave/handleAction/update/render
- `ScreenStack` manages push/pop navigation
- Only the top screen receives events and renders

## Input

- `InputManager` polls SDL events and converts to logical `Action` values
- Raw events are logged for the diagnostics screen
- Key mapping is tentative and will be confirmed on-device

## Directory Structure

The public screen/API/download classes remain stable interfaces. Their implementations are split
by concern into translation units so that lifecycle and coordination code stays small while the
existing behavior and ownership model remain unchanged.

```
src/
  main.cpp, app/                 Application, ScreenStack, diagnostics
  input/                         SDL input polling and logical Actions
  ui/                            Shared UI models, artwork/layout helpers, font
  ui/screens/
    HomeScreen.cpp               HomeScreen lifecycle/coordinator core
    HomeScreenNavigation.cpp     Home navigation and input
    HomeScreenRefresh.cpp        Lightweight refresh work
    HomeScreenSync.cpp           Library synchronization worker
    HomeScreenHierarchy.cpp      Series/season hierarchy work
    HomeScreenArtwork.cpp        Artwork/decode worker work
    HomeScreenSettings.cpp       Settings-tab behavior
    HomeScreenDownloads.cpp      Downloads-tab behavior
    HomeScreenRender.cpp         Home rendering
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
    DownloadReconcile.cpp        Reconcile policy
  playback/                      Playback request and offline journal
include/miyoofin/                 Public identity/version headers
tests/test_main.cpp               Single test harness and dispatch
tests/cases/*.inc                 Included test groups; one translation unit
assets/, distributions/, docs/    Runtime assets, OnionOS packaging, documentation
output/                           Gitignored build artifacts
```

## Threading and persistence boundaries

SDL event handling, screen updates, and rendering remain nonblocking. HTTP requests, curl
transfers, retry delays, artwork decoding, cache scans, removable-storage work, and potentially
blocking worker joins stay on the existing bounded background workers. Screen-owned worker state
continues to use the existing cancellation and lifetime rules.

The refactor preserves all behavior-facing formats and identities: session files, library/image
cache formats, download manifests/indexes and `.part` recovery, playback request/result files,
offline journals, Jellyfin request formats, TLS policy, and canonical public/LAN route fallback.
The split translation units do not create new protocols or alter synchronization ownership.

## Dependency Graph

```
main.cpp
  └─ App / ScreenStack / InputManager
       └─ stable screen classes
            ├─ HomeScreen + concern-specific HomeScreen translation units
            ├─ EpisodeBrowserScreen + rendering/artwork/playback/download units
            ├─ JellyfinApi + JSON/auth/library/hierarchy/playback/download units
            └─ DownloadManager + planning/reconcile/transfer worker units
```
