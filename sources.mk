# -------------------------------------------------------------------
# sources.mk — Shared MiyooFin production source list
# Include after PERF_TELEMETRY is set (default 1).
# -------------------------------------------------------------------

SRC_DIR ?= src

TELEMETRY_SRCS :=
TELEMETRY_TEST_SRCS :=
ifeq ($(PERF_TELEMETRY),1)
TELEMETRY_SRCS := \
    $(SRC_DIR)/diagnostics/TelemetryConfig.cpp \
    $(SRC_DIR)/diagnostics/PerformanceTelemetry.cpp \
    $(SRC_DIR)/diagnostics/PerformanceTelemetryRecord.cpp \
    $(SRC_DIR)/diagnostics/PerformanceTelemetryService.cpp \
    $(SRC_DIR)/diagnostics/PerformanceTelemetrySnapshot.cpp \
    $(SRC_DIR)/diagnostics/MftFormat.cpp \
    $(SRC_DIR)/diagnostics/TelemetryWriter.cpp \
    $(SRC_DIR)/diagnostics/LinuxProcessMetrics.cpp
TELEMETRY_TEST_SRCS := $(TELEMETRY_SRCS)
endif

# Production sources used by both host and ARM builds.
# This list intentionally omits telemetry files so PERF_TELEMETRY=0 compiles them out.
MIYOOFIN_PROD_SRCS := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/app/App.cpp \
    $(SRC_DIR)/app/AppSession.cpp \
    $(SRC_DIR)/app/AppPlayback.cpp \
    $(SRC_DIR)/app/RemoteExitSignal.cpp \
    $(SRC_DIR)/catalog/CatalogDb.cpp \
    $(SRC_DIR)/catalog/CatalogDbWrite.cpp \
    $(SRC_DIR)/catalog/CatalogDbSchema.cpp \
    $(SRC_DIR)/catalog/CatalogDbSchemaOps.cpp \
    $(SRC_DIR)/catalog/CatalogDbQuery.cpp \
    $(SRC_DIR)/catalog/CatalogDbSyncState.cpp \
    $(SRC_DIR)/catalog/CatalogDbHierarchy.cpp \
    $(SRC_DIR)/catalog/MediaItemSql.cpp \
    $(SRC_DIR)/catalog/CatalogCompatibility.cpp \
    $(SRC_DIR)/diagnostics/UiDiagnostics.cpp \
    $(SRC_DIR)/app/ScreenStack.cpp \
    $(SRC_DIR)/input/InputManager.cpp \
    $(SRC_DIR)/image/stb_image_impl.cpp \
    $(SRC_DIR)/image/ImageDecoder.cpp \
    $(SRC_DIR)/net/ArtworkUrl.cpp \
    $(SRC_DIR)/net/HttpClient.cpp \
    $(SRC_DIR)/net/JellyfinApi.cpp \
    $(SRC_DIR)/net/JellyfinApiJson.cpp \
    $(SRC_DIR)/net/JellyfinApiAuth.cpp \
    $(SRC_DIR)/net/JellyfinApiLibrary.cpp \
    $(SRC_DIR)/net/JellyfinApiHierarchy.cpp \
    $(SRC_DIR)/net/JellyfinApiPlayback.cpp \
    $(SRC_DIR)/net/JellyfinApiDownload.cpp \
    $(SRC_DIR)/net/JellyfinLibraryEventParse.cpp \
    $(SRC_DIR)/net/JellyfinLibraryEventQueue.cpp \
    $(SRC_DIR)/net/JellyfinLibraryEventSocket.cpp \
    $(SRC_DIR)/net/Session.cpp \
    $(SRC_DIR)/net/DeviceIdentity.cpp \
    $(SRC_DIR)/cache/ImageCache.cpp \
    $(SRC_DIR)/cache/LibraryCache.cpp \
    $(SRC_DIR)/cache/SyncState.cpp \
    $(SRC_DIR)/playback/OfflineLibraryProjection.cpp \
    $(SRC_DIR)/library/OfflineLibraryQuery.cpp \
    $(SRC_DIR)/library/LibrarySync.cpp \
    $(SRC_DIR)/library/LibrarySyncIncremental.cpp \
    $(SRC_DIR)/library/LibrarySyncEvents.cpp \
    $(SRC_DIR)/library/LibraryQuery.cpp \
    $(SRC_DIR)/library/LibraryCoordinator.cpp \
    $(SRC_DIR)/download/DownloadStore.cpp \
    $(SRC_DIR)/download/DownloadManager.cpp \
    $(SRC_DIR)/download/DownloadManagerPlanning.cpp \
    $(SRC_DIR)/download/DownloadManagerReconcileWorker.cpp \
    $(SRC_DIR)/download/DownloadManagerTransfer.cpp \
    $(SRC_DIR)/download/DownloadReconcile.cpp \
    $(SRC_DIR)/download/DownloadSupport.cpp \
    $(SRC_DIR)/net/HlsPlaylist.cpp \
    $(SRC_DIR)/ui/BitmapFont.cpp \
    $(SRC_DIR)/ui/OnScreenKeyboard.cpp \
    $(SRC_DIR)/ui/HomeSettingsModel.cpp \
    $(SRC_DIR)/ui/HomeTabs.cpp \
    $(SRC_DIR)/ui/HomeArtworkPlan.cpp \
    $(SRC_DIR)/ui/screens/HomeScreen.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenNavigation.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenRefresh.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenSync.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenOffline.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenSyncApply.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenHierarchy.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenArtwork.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenSettings.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenDownloads.cpp \
    $(SRC_DIR)/ui/screens/HomeScreenRender.cpp \
    $(SRC_DIR)/ui/screens/StartupScreen.cpp \
    $(SRC_DIR)/ui/screens/ServerEntryScreen.cpp \
    $(SRC_DIR)/ui/screens/ConnectScreen.cpp \
    $(SRC_DIR)/ui/screens/LoginScreen.cpp \
    $(SRC_DIR)/ui/screens/AuthCheckScreen.cpp \
    $(SRC_DIR)/ui/screens/InputDiagnosticsScreen.cpp \
    $(SRC_DIR)/ui/screens/SeriesScreen.cpp \
    $(SRC_DIR)/ui/screens/SeriesScreenWorker.cpp \
    $(SRC_DIR)/ui/screens/SeriesScreenNavigation.cpp \
    $(SRC_DIR)/ui/screens/SeriesScreenRender.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserScreen.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserData.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserRender.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserArtwork.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserPlayback.cpp \
    $(SRC_DIR)/ui/screens/EpisodeBrowserDownloads.cpp \
    $(SRC_DIR)/ui/screens/MovieDetailsScreen.cpp \
    $(SRC_DIR)/ui/screens/MovieDetailsWorker.cpp \
    $(SRC_DIR)/ui/screens/MovieDetailsRender.cpp \
    $(SRC_DIR)/playback/PlaybackRequest.cpp \
    $(SRC_DIR)/playback/OfflinePlaybackJournal.cpp \
    $(SRC_DIR)/update/AppDir.cpp \
    $(SRC_DIR)/update/Sha256.cpp \
    $(SRC_DIR)/update/UpdateInstallPlan.cpp \
    $(SRC_DIR)/update/UpdateInstaller.cpp \
    $(SRC_DIR)/update/UpdateManifest.cpp \
    $(SRC_DIR)/update/UpdateVersion.cpp \
    $(SRC_DIR)/update/UpdateManager.cpp

# Test library sources: derived from the shared production list.
# Excludes application entry points/screens not linked into the test archive
# and adds test-only catalog helpers.
MIYOOFIN_TEST_EXCLUDED_SRCS := \
    $(SRC_DIR)/main.cpp \
    $(SRC_DIR)/app/App.cpp \
    $(SRC_DIR)/app/AppSession.cpp \
    $(SRC_DIR)/app/AppPlayback.cpp \
    $(SRC_DIR)/ui/screens/StartupScreen.cpp \
    $(SRC_DIR)/ui/screens/ConnectScreen.cpp \
    $(SRC_DIR)/ui/screens/AuthCheckScreen.cpp \
    $(SRC_DIR)/ui/screens/InputDiagnosticsScreen.cpp

MIYOOFIN_TEST_SRCS := $(filter-out $(MIYOOFIN_TEST_EXCLUDED_SRCS),$(MIYOOFIN_PROD_SRCS))
MIYOOFIN_TEST_SRCS += $(SRC_DIR)/catalog/CatalogDbTestCommands.cpp
MIYOOFIN_TEST_SRCS += $(TELEMETRY_TEST_SRCS)
