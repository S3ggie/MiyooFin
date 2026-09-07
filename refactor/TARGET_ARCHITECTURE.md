# Target Architecture

The goal is not a rewrite. The goal is to make the existing architecture understandable and
changeable while preserving behavior.

## Strategy

MiyooFin already has important correctness boundaries: local-first cache behavior, offline
projection, LAN/public routing, external playback handoff, download persistence, and bounded
background workers. The refactor preserves those boundaries and reduces oversized translation
units around them.

The safest order is:

1. Add execution guardrails.
2. Extract pure/tested HomeScreen logic from the giant screen header.
3. Mechanically split `HomeScreen.cpp` by concern while keeping one `HomeScreen` class.
4. Mechanically split `EpisodeBrowserScreen.cpp` by concern.
5. Mechanically split `JellyfinApi.cpp` by endpoint family while keeping the public API stable.
6. Mechanically split `DownloadManager.cpp` by worker/planning/reconcile responsibility.
7. Reduce the giant regression-test source without changing test semantics.
8. Update architecture documentation only after the code layout is stable.

## Desired production layout

The exact names may only change through an explicit plan revision. The intended result is:

```text
src/ui/
  HomeSyncState.hpp
  HomeSettingsModel.hpp
  HomeSettingsModel.cpp
  HomeTabs.hpp
  HomeTabs.cpp
  HomeArtworkPlan.hpp
  HomeArtworkPlan.cpp

src/ui/screens/
  HomeScreen.hpp
  HomeScreen.cpp
  HomeScreenNavigation.cpp
  HomeScreenRender.cpp
  HomeScreenDownloads.cpp
  HomeScreenSettings.cpp
  HomeScreenArtwork.cpp
  HomeScreenHierarchy.cpp
  HomeScreenSync.cpp
  HomeScreenRefresh.cpp

  EpisodeBrowserScreen.hpp
  EpisodeBrowserScreen.cpp
  EpisodeBrowserRender.cpp
  EpisodeBrowserArtwork.cpp
  EpisodeBrowserPlayback.cpp
  EpisodeBrowserDownloads.cpp

src/net/
  JellyfinApi.hpp
  JellyfinApi.cpp
  JellyfinApiJson.cpp
  JellyfinApiAuth.cpp
  JellyfinApiLibrary.cpp
  JellyfinApiHierarchy.cpp
  JellyfinApiPlayback.cpp
  JellyfinApiDownload.cpp

src/download/
  DownloadManager.hpp
  DownloadManager.cpp
  DownloadManagerPlanning.cpp
  DownloadManagerReconcileWorker.cpp
  DownloadManagerTransfer.cpp

tests/
  test_main.cpp
  cases/
    ...included test case groups...
```

This plan intentionally keeps the existing public classes and persistent formats. A later,
separately reviewed project can decide whether those classes should be redesigned.

## HomeScreen end state

`HomeScreen` remains the coordinator, but its responsibilities are physically separated:

- lifecycle / top-level update loop
- navigation/input
- rendering
- settings UI
- downloads UI
- artwork decode/cache scheduling
- hierarchy/poster scheduling
- library sync
- lightweight refresh workers

Pure logic currently embedded in `HomeScreen.hpp` moves to small independently testable modules.

## EpisodeBrowserScreen end state

Keep the class and worker topology. Separate:
- rendering
- thumbnail/prefetch worker logic
- playback handoff
- download-plan actions
- core navigation/lifecycle

## JellyfinApi end state

Keep `JellyfinApi` as the stable API surface during this refactor. Split method definitions by
concern so JSON parsing, authentication, library browsing, hierarchy, playback, and download/HLS
logic no longer live in one giant implementation file.

Do not change endpoint semantics while splitting.

## DownloadManager end state

Keep the class, mutex/condition-variable ownership, and durable store behavior. Split:
- queue/config/snapshot core
- planning
- reconcile worker
- transfer worker/HLS transfer

Do not redesign synchronization in the same tasks that move code.

## Test end state

The huge `tests/test_main.cpp` remains one translation unit initially for maximum compatibility,
but large groups are moved into `tests/cases/*.inc` and included from `test_main.cpp`. This keeps
all existing static helpers/macros and test behavior intact while reducing file-navigation burden
for weaker coding agents.

## Explicit non-goals

- no UI redesign
- no new networking library
- no JSON-library migration
- no download format migration
- no new concurrency framework
- no new dependency injection framework
- no style-only cleanup campaign
- no feature work mixed into refactor commits
