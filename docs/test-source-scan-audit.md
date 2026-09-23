# Test production-source scan audit

This audit classifies every remaining test that reads production source code
(as opposed to exercising a runtime seam) and records the disposition of the
source-shape coverage removed during the source-test cleanup.

## Counting method

A **scan site** is one distinct test function (or one shell script) that reads
production source code by any of:

* a raw production-source read in a C/C++ test, whether through the
  `readTestBytes("src/...")` helper or a direct `std::ifstream` of a `src/`
  file,
* a `std::filesystem` directory walk rooted at `src` or a subtree,
* the transitive `auditThreadFile` / `kThreadAuditFiles` file-list audit,
* a shell `grep`/`awk` against `src/...` or a distribution script.

Sites are classified by primary purpose:

| Class | Meaning |
|-------|---------|
| **A** | Architecture / ownership / lifetime / module-boundary invariant with no deterministic host seam. |
| **B** | Behavioural source scan: asserts runtime control flow that is (or should be) covered behaviourally. Target is zero. |
| **C** | Unjustified/brittle source scan with neither an architecture nor a dependency justification. Target is zero. |
| **D** | Dependency-direction guard: a module must not call a forbidden API or depend in a forbidden direction. |

Mixed sites are classified by their dominant purpose.

## Inventory

### C++ `readTestBytes("src/...")` sites — 15 functions, 36 call sites

| # | Site | Class | Justification |
|---|------|-------|---------------|
| 1 | `testSeriesCachedSeasonHandoff` (cache_offline) | D | Series/Episode screens read hierarchy through the coordinator query, not `JellyfinApi::getSeasons/Episodes`. |
| 2 | `testCatalogDbOfflineDownloadReconstruction` (migration) | A | `CatalogDb` is independent of `DownloadStore` / offline reconstruction. |
| 3 | `testLibraryCacheHomeParityHarness` (hierarchy) | A | Coordinator is the single hierarchy authority; Home/HomeScreen own no hierarchy worker or sync-state writes. |
| 4 | `testHomeDefersViewPersistenceToCoordinator` (hierarchy) | D | Home requests `requestFullPopulation`, never top-level `begin/finalize/abort` sync staging. |
| 5 | `testCatalogScopeConfiguredBeforeHomePopulation` (hierarchy) | A | Catalog scope is configured before `goToHome()` (lifecycle ordering). |
| 6 | `testHomeFetchOwnershipGuard` (hierarchy) | A | `HomeScreen` owns the fetch via `unique_ptr<HomeLibraryController>` and declares no fetch thread; the controller owns the single thread. |
| 7 | `testCatalogDbBoundedMediaPaging` (query) | A | `CatalogDb` stays free of `JellyfinApi`/`LibraryCache`/`TitleOrganization`; Home never uses the legacy snapshot surface. |
| 8 | `testHomeUsesCatalogBeforeNetworkRefresh` (query) | D | The Home sync worker never calls `configureScope`; scope is coordinator-owned. |
| 9 | `testLibraryQueryDomainBoundary` (query) | D | `LibraryQuery` is constructed only by the coordinator and exposes only domain types. |
| 10 | `testCatalogDbLibrarySnapshotSeed` (sync) | A | `CatalogDb` has no snapshot seed/read API; `CatalogCompatibility` owns it. |
| 11 | `testNonBlockingFinishPolicy` (misc) | A | `finish*` handlers never join a worker; only `start*` handlers join. |
| 12 | `testControllerHasNoHomeOrSdlDependency` (home_artwork_controller) | A | `HomeArtworkController` has no HomeScreen/SDL/LibrarySync dependency. |
| 13 | `testLibraryCoordinatorIsTheSingleStartupDriver` (library_coordinator) | D | Consumers never call raw `take*` result APIs, raw sync staging, or `coordinator->stop()`; teardown orders stop before join. |
| 14 | `testLiveChangesWaitForSerializedSyncSlots` (library_coordinator) | A | Home owns no live-change worker thread. |
| 15 | `testSafetyReconcilePublishesCoordinatorResult` (library_coordinator) | D | Home requests maintenance; it never drives safety reconcile itself or owns that worker. |

### Other production-source scan sites

| # | Site | Class | Justification |
|---|------|-------|---------------|
| 16 | `testNetworkLayerDoesNotDependOnPresentationModels` (`src/net` walk) | A | Network layer has no `TabData`/`MediaRow`/presentation-model references. |
| 17 | `testLibraryQueryDomainBoundary` (`src` walk) | D | No TU outside the coordinator/query may construct `LibraryQuery`. (Directory walk, same function as #9.) |
| 18 | `testThreadRestartGuards` (`kThreadAuditFiles`, 18 files) | A | No unguarded `std::thread` restart (SIGABRT pin). |
| 19 | `testNoDetachedProductionThreads` (`kThreadAuditFiles`, 18 files) | A | No `.detach()`/`pthread_detach()` in audited production TUs. |
| 20 | `test_media_item_header.sh` | A | `MediaItem` is independent of SDL/UI and placeholder-artwork types. |
| 21 | `test_onion_remote_launcher.sh` | A | Online Home startup has no full-snapshot read/blocking seed; catalog scope precedes Home. |
| 22 | `test_playback_runner.sh` | A | Runner must not force MiyooFin SDL drivers onto FFplay. |
| 23 | `test_library_sync_guard.sh` (via `tools/check-library-sync-*.sh`) | D | `LibrarySync` construction and UI/app include boundary. Operates on synthetic fixtures, not the real tree. |
| 24 | `test_desktop_runtime.sh` | D | Desktop/Onion mode-selection wiring guard: the launcher must select desktop input/window/playback modes, the runner must keep its Onion default, and the host build must emit header dependencies. The desktop branch's case dispatch, driver clearing, and configurable FFplay binary are asserted by the executable fixture, not by a source scan. |
| 25 | `test_ca_bundle.sh` | D | No first-party production or tooling unit may bypass libcurl certificate verification (`CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST 0L`, `--insecure`, `-k`, `--no-check-certificate`). Forbidden-API guard over `src`, `tools`, and `distributions/onionos`; the same script's Makefile packaging checks are build wiring and are not production-source scans. |

### Out-of-scope shell checks

`test_developer_ssh.sh` and `test_release_legal.sh` grep `tools/*.sh`,
`tools/miyoo/*` C helpers, `LICENSE`, and `THIRD_PARTY_NOTICES.md`, but never
`src/...` or a `distributions/` script, so they do not meet the counting
method above. `tools/refactor-check.sh` does scan `src/`, but it is a CI
refactor gate rather than a test in `make test`; it is tracked separately from
this test-scan inventory.

## Counts

| Class | Sites |
|-------|------:|
| A | 15 |
| B | 0 |
| C | 0 |
| D | 10 |
| **Total** | **25** |

Note: sites 9 and 17 are the same test function counted once for its
`readTestBytes` scan and once for its directory walk, so the table lists 25
rows but 24 distinct test/script units.

B and C are zero for the retained sites as an enumerated result rather than an
assumption. Two facts support it:

* The only class-B behavioural source scan in the tree,
  `testLibraryStartupTimelineMarkers`, read `src/catalog/CatalogDb.cpp` through
  a raw `std::ifstream` and asserted the `read_media_page_dequeued` /
  `read_media_page_ready` markers. It passed on a source comment because the
  markers are actually emitted in `CatalogDbQuery.cpp`. It has been removed and
  replaced by a behavioural `CatalogDb::readMediaPage` read observed through
  the `UiDiagnostics` persistence seam (see the removed-coverage table below).
* The counting method now includes raw `std::ifstream` source reads, so a
  reintroduced scan of that shape is counted instead of silently omitted.

`test_desktop_runtime.sh` previously carried class-B behavioural scans (an
indentation-sensitive `desktop)` case guard, a bare `ffplay` presence check, a
`MIYOOFIN_FFPLAY_BIN` presence check, and two `unset SDL_*` driver checks).
Those are removed and replaced by the executable fixture, which runs the
desktop branch with inherited Onion driver names and asserts the configured
FFplay binary observed them cleared. The retained source scans are class A/D.
Any future behavioural control-flow assertion must be expressed as a runtime
test rather than a source scan.

## Removed-coverage disposition

| Removed area | Disposition |
|--------------|-------------|
| Downloads I/O / fsync / race ordering (`testDownloadPipelineIoGuards`, `testAbandonedDeleteFlagConsumed`, `testStaleSegmentRemovalUnderLock`, `testDequeuePersistIsPerItem`) | Not restored. Call ordering/fsync cost is not host-observable; the decisions they serve are covered behaviourally by `testWorkerPersistMatchesLiveState`, `testTransferFinishDecision`, `testPersistTouchesOnlyAffectedItem`, `testPersistPendingCrashDurabilityAndTmpSweep`, `testSegmentRecoveryMatchesIncremental`, `testStalePlaylistDiscoverySuppressed`. |
| Home navigation (`testHomeStartupSyncSourceOrder`, tab-shape pins) | Restored behaviourally as `testHomeTabNavigation` (shoulder-button tab moves and wrap). |
| Home offline toggle (`testOfflineModeToggleDrivesFetchPath`, `testOfflineToggleCancelsFetchAndUsesCache`) | Controller-side offline publication covered by `testOfflineFetchPublishesDownloadedPresentation`; signature semantics by `testOfflineSnapshotSignatureStability`. The Settings toggle handler itself needs a live session/SDL and is not host-observable. |
| Home cold provisional / rail-only / publication ordering | Restored behaviourally via the `MIYOOFIN_TEST_BUILD` HomeScreen seam (`applyFetchedPresentationForTest`): `testHomeDiscardsColdProvisionalFailure`, `testHomeKeepsRailOnlyColdStartLoading`, `testHomePendingCompletionPublicationOrdering`. |
| Hierarchy teardown | Home/HomeScreen ownership guards retained in `testLibraryCacheHomeParityHarness`; coordinator hierarchy lifecycle covered by `test_library_hierarchy` and the coordinator tests. |
| Playback SDL order (`testPlaybackRunnerInitializesOnionSdlDrivers`, `testExternalPlaybackFullyReleasesSdlBeforeExec`) | Runner driver contract covered by `test_playback_runner.sh`; SDL release-before-fork has no host seam and is not restored. |
| Desktop mappings/dispatch/diagnostics | Desktop branch dispatch, driver clearing, and configurable FFplay binary are exercised behaviourally by the `test_desktop_runtime.sh` fixture; launcher env and ui-script diagnostic routing remain wiring scans (site 24). SDL scancode mappings and pointer dispatch have no host behavioural seam. |
| CatalogDb startup timeline source scan (`testLibraryStartupTimelineMarkers` reading `src/catalog/CatalogDb.cpp`) | Restored behaviourally as `testLibraryStartupTimelineMarkers` driving a real `CatalogDb::readMediaPage` and asserting the `read_media_page_dequeued` / `read_media_page_ready` lines persisted through `UiDiagnostics`. |
