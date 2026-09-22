#include "HomeScreen.hpp"
#include <chrono>
#include "../../catalog/CatalogDb.hpp"
#include "../../library/OfflineLibraryQuery.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../ArtworkLayout.hpp"
#include <cstdio>
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs()
{
    return (std::int64_t)std::time(nullptr) * 1000;
}
void HomeScreen::requestMediaPage(MediaPageState& state)
{
    if (state.inFlight || !state.hasMore)
        return;
    state.cancellation = std::make_shared<std::atomic_bool>(false);
    if (presentationOffline()) {
        std::promise<library::MediaPage> page;
        page.set_value(offlineMediaPage(m_offlineSnapshot, state.type, state.letter,
                                        state.type == "anime" ? 64 : 24, state.next));
        state.future = page.get_future();
        state.inFlight = true;
        return;
    }
    if (!m_libraryQuery)
        return;
    if (state.type == "movie")
        state.future = m_libraryQuery->movies(state.letter, 24, state.next, state.cancellation);
    else if (state.type == "anime")
        state.future = m_libraryQuery->anime(state.letter, 64, state.next);
    else
        state.future = m_libraryQuery->shows(state.letter, 24, state.next, state.cancellation);
    state.inFlight = true;
    if (!m_firstMediaPageReadLogged) {
        m_firstMediaPageReadLogged = true;
        uiDiagnostics().log("[HomeScreen] startup stage=first_read_media_page_requested");
    }
}

void HomeScreen::requestEarlierMediaPage(MediaPageState& state)
{
    if (!m_libraryQuery || state.inFlight || !state.hasEarlier)
        return;
    const std::string type = state.type;
    const int letter = state.letter;
    if (state.cancellation)
        state.cancellation->store(true);
    state = MediaPageState{};
    state.type = type;
    state.letter = letter;
    state.replaceWindowOnNextPage = true;
    requestMediaPage(state);
}

void HomeScreen::updateMediaPaging()
{
    finishMediaPage(m_moviePage);
    finishMediaPage(m_showPage);
    finishMediaPage(m_animePage);
    if (activeTabNamed("Movies")) {
        const auto& rows = m_tabs[tabIndex("Movies")].rows;
        const auto& items = rows.empty() ? m_movieWindow : rows[0].items;
        if (items.empty() || m_activeCard + 8 >= static_cast<int>(items.size()))
            requestMediaPage(m_moviePage);
    } else if (activeTabNamed("Shows")) {
        const int showCount = static_cast<int>(m_filteredShows.size());
        const int animeCount = static_cast<int>(m_filteredAnime.size());
        if (showCount == 0 || m_showSelected + 4 >= showCount)
            requestMediaPage(m_showPage);
        if (animeCount == 0 || m_animeSelected + 4 >= animeCount)
            requestMediaPage(m_animePage);
    }
}

void HomeScreen::updateLiveLibraryChanges()
{
    if (!m_libraryCoordinator || presentationOffline())
        return;

    library::LiveLibraryChangeResult result;
    if (!m_libraryCoordinator->takeLiveChangeResult(result))
        return;

    if (result.success && liveChangeAffectsHome(result))
        publishLiveCatalogItems(result);

    if (result.success && result.userDataChanged) {
        const std::int64_t nowMs = wallClockMs();
        if (!homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshCompletedMs) &&
            !homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshAttemptMs)) {
            m_homeSyncActive = true;
            startHomeRailRefresh();
        }
    }
}

void HomeScreen::startHomeRailRefresh()
{
    if (m_homeRailRefreshInFlight) {
        m_homeRailRefreshPending = true;
        return;
    }
    if (!m_libraryCoordinator)
        return;
    m_homeRailRefreshDone.store(false);
    std::uint64_t request = 0;
    if (!m_libraryCoordinator->requestHomeRailRefresh(request))
        return;
    m_homeRailRefreshRequest = request;
    m_homeRailRefreshInFlight = true;
    m_homeRailRefreshSucceeded = false;
    m_lastHomeRailRefreshAttemptMs = wallClockMs();
    m_homeRailContinueValid = false;
    m_homeRailRecentValid = false;
    m_homeRailContinueWatching.clear();
    m_homeRailRecentlyAdded.clear();
    m_homeRailRefreshError.clear();
}

bool HomeScreen::startFetch()
{
    if (m_fetchThread.joinable()) {
        // If the previous fetch completed, join its thread to reclaim it.
        // If it hasn't completed yet, don't start a competing fetch.
        if (m_fetchComplete.load() || m_fetchDone.load())
            m_fetchThread.join();
        else
            return false;
    }
    m_metadataCompleted.store(0);
    m_metadataTotal.store(0);
    m_metadataActive.store(true);
    m_artworkPlanningComplete.store(false);
    m_fetchPreviousTabs = m_tabs;
    m_fetchPreviousCachedSnapshot = m_cachedSnapshot;
    m_fetchPreviousRemoteSnapshot = m_remoteSnapshot;
    m_fetchPreviousHaveCachedSnapshot = m_haveCachedSnapshot;
    m_fetchPreviousLibraryOffline = m_libraryOffline;
    m_fetchPreviousContentValid = m_loadState == LoadState::Ready && !m_tabs.empty();
    m_fetchDone = false;
    m_fetchReady.store(false);
    m_fetchComplete.store(false);
    m_fetchPublished = false;
    m_fetchPostFinalizeApplied = false;
    m_fetchFailureRestored = false;
    m_fetchCatalogCommitted.store(false);
    m_fetchError.clear();
    m_fetchResult.clear();
    m_fetchCacheSaved = false;
    m_fetchOfflinePrepared = false;
    m_homeRailsReady.store(false);
    m_homeRailsApplied = false;
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        m_animeItemIds.clear();
    }
    m_fetchCancellation = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> cancellation = m_fetchCancellation;
    Session session = m_session;
    m_fetchThread = std::thread([this, session, cancellation]() {
        PerformanceTelemetry& telemetry = performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, true);
        telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
        PendingPresentation pending;
        pending.remoteSnapshot = m_fetchPreviousRemoteSnapshot;
        pending.haveCachedSnapshot = m_fetchPreviousHaveCachedSnapshot;
        pending.cachedSnapshot = m_fetchPreviousCachedSnapshot;
        pending.libraryOffline = m_fetchPreviousLibraryOffline;
        auto publish = [&]() { publishPendingPresentation(pending); };
        CatalogDbJobMetadata metadata;
        metadata.scopeEpoch = catalogScopeEpoch();
        metadata.cancellation = cancellation;
        // Online Home readiness must not wait on CatalogDb, even for the
        // small sync checkpoint.  The checkpoint is advisory here; the
        // bounded Home reads below are the only startup data dependency.
        // Background sync/reconcile paths can refresh these fields later.
        TelemetryTimer syncTimer;
        uint32_t requestCount = 0;
        uint32_t changedHierarchyCount = 0;
        uint32_t mediaCount = 0;
        bool cacheSaved = false;
        bool completed = false;
        std::vector<LibraryView> views;
        auto completeTelemetry = [&](Outcome outcome) noexcept {
            if (completed)
                return;
            completed = true;
            if (syncTimer.active() && telemetry.enabledFast()) {
                TelemetryRecord record{};
                record.header.record_type = RecordType::LibrarySync;
                record.payload.library_sync.duration_us = syncTimer.elapsedUs();
                record.payload.library_sync.outcome = static_cast<uint8_t>(outcome);
                record.payload.library_sync.cache_saved = cacheSaved ? 1 : 0;
                record.payload.library_sync.views_count = static_cast<uint32_t>(views.size());
                record.payload.library_sync.media_count = mediaCount;
                record.payload.library_sync.changed_hierarchy_count = changedHierarchyCount;
                record.payload.library_sync.request_count = requestCount;
                telemetry.emitRecord(record);
            }
            telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, false);
            telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 0);
        };
        if (session.manualOfflineMode) {
            const DownloadSnapshot downloads =
                m_downloads ? m_downloads->snapshot() : DownloadSnapshot{};
            std::vector<MediaItem> metadataItems;
            if (m_libraryQuery) {
                for (const auto& batch : OfflineLibraryQuery::metadataBatches(downloads)) {
                    if (cancellation->load())
                        return;
                    const auto metadataPage = m_libraryQuery->itemsByIds(batch, cancellation).get();
                    if (metadataPage.cancelled || metadataPage.superseded)
                        return;
                    if (metadataPage.success) {
                        metadataItems.insert(metadataItems.end(), metadataPage.items.begin(),
                                             metadataPage.items.end());
                    }
                }
            }
            pending.cachedSnapshot = OfflineLibraryQuery::build(downloads, metadataItems);
            pending.haveCachedSnapshot = true;
            pending.libraryOffline = true;
            // Cache the offline snapshot + its signature for instant
            // reuse when the user toggles offline mode again without
            // download changes.
            pending.offlineSnapshotCache = pending.cachedSnapshot;
            pending.offlineCacheValid = true;
            pending.offlineSignature =
                computeOfflineSignature(downloads, committedCatalogGeneration());
            pending.tabs = offlineTabsFromSnapshot(pending.cachedSnapshot);
            pending.remoteSnapshot = pending.cachedSnapshot;
            pending.cacheSaved = true;
            pending.contentValid = true;
            pending.continueValid = true;
            pending.recentlyAddedValid = true;
            pending.continueWatching = pending.cachedSnapshot.continueWatching;
            pending.recentlyAdded = pending.cachedSnapshot.recentlyAdded;
            pending.complete = true;
            pending.diagnosticStage = "offline_terminal";
            publish();
            m_metadataActive.store(false);
            completeTelemetry(Outcome::Success);
            m_fetchDone = true;
            return;
        }
        bool optionalRailFailed = false;
        bool catalogRefreshFailed = false;
        bool initialPagePublished = false;
        bool firstBoundedRequestLogged = false;
        bool firstPagePersistedLogged = false;
        bool coordinatorStartupStarted = false;
        library::StartupSyncResult startupSyncResult;
        uiDiagnostics().log("[HomeScreen] startup stage=home_fetch_started");
        // Probe the committed catalog before starting any network refresh. These
        // are bounded reads on the CatalogDb worker; the SDL thread only sees
        // the publication signal in update(). A genuinely empty catalog must
        // remain Loading until the authoritative network generation commits.
        if (m_libraryQuery && catalogScopeEpoch() != 0) {
            auto warmMovies = m_libraryQuery->movies(-1, 24, {}, cancellation);
            auto warmShows = m_libraryQuery->shows(-1, 24, {}, cancellation);
            const auto movies = warmMovies.get();
            const auto shows = warmShows.get();
            // Keep this source-level predicate contiguous for the parity guard.
            // clang-format off
            if (!cancellation->load() && !movies.cancelled && !shows.cancelled
                && !movies.superseded && !shows.superseded
                && (!movies.items.empty() || !shows.items.empty())) {
                // clang-format on
                std::vector<TabData> warmTabs;
                warmTabs.push_back({"Home", {{"", {}}}});
                if (!movies.items.empty())
                    warmTabs.push_back({"Movies", {{"Movies", movies.items}}});
                else
                    warmTabs.push_back({"Movies", {{"Movies", {}}}});
                if (!shows.items.empty())
                    warmTabs.push_back({"Shows", {{"Shows", shows.items}}});
                else
                    warmTabs.push_back({"Shows", {{"Shows", {}}}});
                warmTabs.push_back({"Downloads", {{"", {}}}});
                warmTabs.push_back({"Settings", {{"", {}}}});
                {
                    std::lock_guard<std::mutex> lock(m_fetchMutex);
                    for (const auto& item : shows.items) {
                        const auto found = shows.membershipsByItem.find(item.id);
                        if (found == shows.membershipsByItem.end())
                            continue;
                        for (const auto& membership : found->second) {
                            if (isAnimeSeries(membership.viewName, item)) {
                                m_animeItemIds.insert(item.id);
                                break;
                            }
                        }
                    }
                    pending.tabs = std::move(warmTabs);
                }
                std::vector<TabData> warmTabsForState;
                {
                    std::lock_guard<std::mutex> lock(m_fetchMutex);
                    warmTabsForState = pending.tabs;
                }
                pending.tabs = std::move(warmTabsForState);
                pending.contentValid = true;
                pending.stale = true;
                publish();
                uiDiagnostics().log("[HomeScreen] startup stage=warm_sqlite_catalog_ready");
                initialPagePublished = true;
            }
        }
        // Block live-change rail refreshes until the startup population
        // walk commits so a queued UserDataChanged cannot start a
        // competing rail refresh during the startup window. This atomic must
        // be published before the coordinator can launch its worker: the UI
        // thread may process a library event concurrently with this worker.
        m_initialPopulationInProgress = true;
        // The coordinator reads the persisted checkpoint and owns the startup
        // policy. Home waits for that result before deciding whether to retain
        // the old full-population walk, so no two top-level syncs overlap.
        if (m_libraryCoordinator)
            coordinatorStartupStarted =
                m_libraryCoordinator->startStartupSync(initialPagePublished);
        // Ask the coordinator-owned rail worker for Continue Watching /
        // Recently Added immediately after the warm CatalogDb probe.  Home
        // consumes the immutable publication but never performs these
        // network requests itself.
        std::vector<MediaItem> cw;
        std::string cwErr;
        std::vector<MediaItem> ra;
        std::string raErr;
        bool cwOk = pending.haveCachedSnapshot;
        bool raOk = pending.haveCachedSnapshot;
        if (pending.haveCachedSnapshot) {
            cw = pending.cachedSnapshot.continueWatching;
            ra = pending.cachedSnapshot.recentlyAdded;
            pending.remoteSnapshot = pending.cachedSnapshot;
        }
        uiDiagnostics().log("[HomeScreen] startup stage=continue_watching_started");
        std::uint64_t railRequest = 0;
        const bool railStarted =
            m_libraryCoordinator && m_libraryCoordinator->requestHomeRailRefresh(railRequest);
        library::HomeRailResult railResult;
        if (railStarted) {
            ++requestCount;
            for (;;) {
                if (cancellation->load())
                    m_libraryCoordinator->cancelHomeRailRefresh();
                if (m_libraryCoordinator->takeHomeRailResult(railRequest, railResult))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (railResult.continueValid) {
                cwOk = true;
                cw = railResult.continueWatching;
            } else {
                optionalRailFailed = true;
                cwErr = railResult.error;
            }
            if (railResult.recentlyAddedValid) {
                raOk = true;
                ra = railResult.recentlyAdded;
            } else {
                optionalRailFailed = true;
                raErr = railResult.error;
            }
        }
        if (!railStarted || !cwOk) {
            // Keep the assignment marker contiguous for the parity guard.
            // clang-format off
            optionalRailFailed=true;
            // clang-format on
            printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());
        }
        uiDiagnostics().log("[HomeScreen] startup stage=continue_watching_finished");
        uiDiagnostics().log("[HomeScreen] startup stage=recently_added_started");
        if (!railStarted || !raOk) {
            // Keep the assignment marker contiguous for the parity guard.
            // clang-format off
            optionalRailFailed=true;
            // clang-format on
            printf("[HomeScreen] Recently added: %s\n", raErr.c_str());
        }
        uiDiagnostics().log("[HomeScreen] startup stage=recently_added_finished");
        pending.railsReady = true;
        pending.continueWatching = cw;
        pending.recentlyAdded = ra;
        pending.continueValid = cwOk;
        pending.recentlyAddedValid = raOk;
        pending.remoteSnapshot.continueWatching = cw;
        pending.remoteSnapshot.recentlyAdded = ra;
        publish();
        queuePosterJobs(planHomeRailPosterJobs(cw, ra), true);
        if (coordinatorStartupStarted) {
            for (;;) {
                if (cancellation->load())
                    m_libraryCoordinator->cancelStartupSync();
                if (m_libraryCoordinator->takeStartupSyncResult(startupSyncResult))
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        } else {
            // Compatibility construction without a coordinator retains the
            // existing safe fallback: only the full population path runs.
            startupSyncResult.mode = library::StartupSyncMode::FullReconcile;
        }
        // Accumulate fetched items per collection type so the
        // post-finalize rebuild can populate Home tab items.
        std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
        std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
        bool forceHierarchyReconcile =
            startupSyncResult.mode == library::StartupSyncMode::FullReconcile;
        // FIX 2: guard so that if any exception escapes the population
        // walk the deferral flag is cleared and poster workers are woken,
        // preventing a permanent low-priority artwork stall.
        try {
            bool deltaCatchUpSucceeded = false;
            if (startupSyncResult.mode == library::StartupSyncMode::SkipFresh) {
                // Catalog is within the FRESH_MS window — skip the walk entirely.
                if (cwOk)
                    pending.remoteSnapshot.continueWatching = cw;
                if (raOk)
                    pending.remoteSnapshot.recentlyAdded = ra;
            } else if (startupSyncResult.mode == library::StartupSyncMode::DeltaCatchUp) {
                // The coordinator already performed the bounded delta catch-up.
                uiDiagnostics().log("[HomeScreen] startup stage=delta_catchup_started");
                if (startupSyncResult.success && !startupSyncResult.cancelled &&
                    !startupSyncResult.superseded) {
                    if (cwOk)
                        pending.remoteSnapshot.continueWatching = cw;
                    if (raOk)
                        pending.remoteSnapshot.recentlyAdded = ra;
                    deltaCatchUpSucceeded = true;
                } else if (startupSyncResult.cancelled || startupSyncResult.superseded) {
                    // Cancellation or superseded — treat as SkipFresh: rails +
                    // warm catalog already available, no full walk, no error.
                    if (cwOk)
                        pending.remoteSnapshot.continueWatching = cw;
                    if (raOk)
                        pending.remoteSnapshot.recentlyAdded = ra;
                    deltaCatchUpSucceeded = true;
                } else {
                    // Catch-up failed — fall through to full reconcile.
                    forceHierarchyReconcile = true;
                }
                uiDiagnostics().log("[HomeScreen] startup stage=delta_catchup_finished");
            }
            if (startupSyncResult.mode == library::StartupSyncMode::FullReconcile ||
                (startupSyncResult.mode == library::StartupSyncMode::DeltaCatchUp &&
                 !deltaCatchUpSucceeded)) {
                if (!m_libraryCoordinator) {
                    catalogRefreshFailed = true;
                    pending.error = "Library coordinator unavailable";
                }
                if (!catalogRefreshFailed) {
                    uiDiagnostics().log("[HomeScreen] startup stage=views_started");
                    std::uint64_t populationRequest = 0;
                    if (!m_libraryCoordinator->requestFullPopulation(populationRequest)) {
                        catalogRefreshFailed = true;
                        pending.error = "Library sync already in flight";
                    } else {
                        bool populationComplete = false;
                        std::size_t populationMissCount = 0;
                        std::size_t populationCompletedPages = 0;
                        bool populationMissLogged = false;
                        bool populationIdentityMismatchLogged = false;
                        std::string populationConsumerStage;
                        while (!populationComplete) {
                            if (cancellation->load())
                                m_libraryCoordinator->cancelFullPopulation();
                            library::FullPopulationUpdate update;
                            if (!m_libraryCoordinator->takeFullPopulationUpdate(populationRequest,
                                                                                update)) {
                                if (populationMissCount < 1000000)
                                    ++populationMissCount;
                                const auto status = m_libraryCoordinator->status();
                                const bool identityMismatch =
                                    status.fullPopulationRequest != populationRequest;
                                if (!populationMissLogged ||
                                    (identityMismatch && !populationIdentityMismatchLogged)) {
                                    uiDiagnostics().log(
                                        "[HomeScreen] full_population_consumer miss"
                                        " requested_request=" +
                                        std::to_string(populationRequest) + " current_request=" +
                                        std::to_string(status.fullPopulationRequest) +
                                        " current_generation=" +
                                        std::to_string(status.fullPopulationGeneration) +
                                        " identity_mismatch=" +
                                        std::to_string(identityMismatch ? 1 : 0) + " miss_count=" +
                                        std::to_string(populationMissCount) + " queue_depth=" +
                                        std::to_string(status.fullPopulationQueueDepth));
                                    populationMissLogged = true;
                                    populationIdentityMismatchLogged =
                                        populationIdentityMismatchLogged || identityMismatch;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                continue;
                            }
                            const std::string consumerStage =
                                update.terminal
                                    ? (update.success ? "terminal_success"
                                                      : (update.cancelled || update.superseded
                                                             ? "terminal_cancel"
                                                             : "terminal_failure"))
                                    : (update.firstPage ? "first_page" : "page");
                            if (update.request != populationRequest) {
                                uiDiagnostics().log(
                                    "[HomeScreen] full_population_consumer identity_mismatch"
                                    " requested_request=" +
                                    std::to_string(populationRequest) + " current_request=" +
                                    std::to_string(update.request) + " current_update_generation=" +
                                    std::to_string(update.generation) +
                                    " miss_count=" + std::to_string(populationMissCount));
                            }
                            if (consumerStage != populationConsumerStage) {
                                uiDiagnostics().log(
                                    std::string("[HomeScreen] full_population_consumer ") +
                                    (populationConsumerStage.empty() ? "hit" : "transition") +
                                    " requested_request=" + std::to_string(populationRequest) +
                                    " current_request=" + std::to_string(update.request) +
                                    " current_update_generation=" +
                                    std::to_string(update.generation) + " stage=" + consumerStage +
                                    " miss_count=" + std::to_string(populationMissCount));
                                populationConsumerStage = consumerStage;
                            }
                            if (update.pageValid)
                                ++populationCompletedPages;
                            pending.diagnosticRequest = populationRequest;
                            pending.diagnosticGeneration = update.generation;
                            pending.diagnosticCompletedPages = populationCompletedPages;
                            pending.diagnosticStage = consumerStage;
                            if (!update.views.empty())
                                views = update.views;
                            requestCount = update.requestCount;
                            mediaCount = update.mediaCount;
                            m_metadataTotal.store(update.metadataTotal);
                            m_metadataCompleted.store(update.metadataCompleted);
                            if (update.pageValid) {
                                const auto& page = update.page;
                                const auto& view = update.view;
                                if (!firstBoundedRequestLogged) {
                                    firstBoundedRequestLogged = true;
                                    uiDiagnostics().log(
                                        "[HomeScreen] startup stage=first_bounded_request_started");
                                }
                                if (view.collectionType == "tvshows") {
                                    std::lock_guard<std::mutex> lock(m_fetchMutex);
                                    for (const auto& item : page.items)
                                        if (isAnimeSeries(view.name, item))
                                            m_animeItemIds.insert(item.id);
                                }
                                std::printf(
                                    "[HomeScreen] page_validated start=%d count=%zu more=%d\n",
                                    page.startIndex, page.items.size(), page.hasMore ? 1 : 0);
                                queuePosterJobs(planMediaPagePosterJobs(page.items),
                                                update.firstPage);
                                auto& targetList =
                                    view.collectionType == "tvshows" ? showsByView : moviesByView;
                                if (targetList.empty() || targetList.back().first != view.name)
                                    targetList.emplace_back(view.name, std::vector<MediaItem>{});
                                auto& items = targetList.back().second;
                                if (items.size() < 24) {
                                    const std::size_t count =
                                        std::min<std::size_t>(24 - items.size(), page.items.size());
                                    items.insert(items.end(), page.items.begin(),
                                                 page.items.begin() +
                                                     static_cast<std::ptrdiff_t>(count));
                                }
                                if (!firstPagePersistedLogged) {
                                    firstPagePersistedLogged = true;
                                    uiDiagnostics().log(
                                        "[HomeScreen] startup stage=first_page_persisted");
                                }
                                if (update.firstPage && !initialPagePublished) {
                                    std::vector<TabData> firstPageTabs;
                                    {
                                        std::lock_guard<std::mutex> lock(m_fetchMutex);
                                        pending.tabs = buildTabs(cw, ra, moviesByView, showsByView);
                                        firstPageTabs = pending.tabs;
                                    }
                                    LibrarySnapshot firstPageSnapshot;
                                    firstPageSnapshot.continueWatching = cw;
                                    firstPageSnapshot.recentlyAdded = ra;
                                    // This is a useful provisional frame, not an
                                    // authoritative catalog publication.  The
                                    // coordinator retains the prior committed Home
                                    // state until the full generation finalizes.
                                    pending.tabs = std::move(firstPageTabs);
                                    pending.remoteSnapshot = firstPageSnapshot;
                                    pending.contentValid = true;
                                    pending.continueValid = cwOk;
                                    pending.recentlyAddedValid = raOk;
                                    pending.continueWatching = cw;
                                    pending.recentlyAdded = ra;
                                    publish();
                                    std::printf("[HomeScreen] first bounded page ready views=%zu\n",
                                                views.size());
                                    initialPagePublished = true;
                                    uiDiagnostics().log(
                                        "[HomeScreen] startup stage=first_bounded_page_ready");
                                }
                            }
                            if (!update.terminal)
                                continue;
                            populationComplete = true;
                            views = std::move(update.views);
                            moviesByView = std::move(update.moviesByView);
                            showsByView = std::move(update.showsByView);
                            if (!update.success && !update.committed) {
                                catalogRefreshFailed = true;
                                pending.error = update.message.empty()
                                                    ? (update.cancelled || update.superseded
                                                           ? "Library refresh cancelled"
                                                           : "Library refresh failed")
                                                    : update.message;
                            }
                            if (update.committed)
                                m_fetchCatalogCommitted.store(true);
                            if (update.committed)
                                pending.tabs = buildTabs(cw, ra, moviesByView, showsByView);
                            if (update.success)
                                uiDiagnostics().log("[HomeScreen] startup stage=views_finished");
                        }
                    }
                }
            } // FullReconcile scope
        } catch (...) {
            // FIX 2: On exception, guarantee the deferral flag is
            // cleared and poster workers are woken so the low-priority
            // artwork backlog can drain.
            m_initialPopulationInProgress = false;
            {
                std::lock_guard<std::mutex> lock(m_posterMutex);
            }
            m_posterWake.notify_all();
            throw;
        }
        // FIX 3: Store the deferral flag under m_posterMutex so the
        // ordering between clearing the flag and waking poster workers
        // is self-evident — the mutex pairs this store with the wait
        // predicate that reads it.
        {
            std::lock_guard<std::mutex> lock(m_posterMutex);
            m_initialPopulationInProgress = false;
        }
        // Wake poster workers so they begin draining any deferred
        // low-priority artwork jobs that were held back during the
        // population walk.
        m_posterWake.notify_all();
        if (catalogRefreshFailed && pending.error.empty())
            pending.error = "Library refresh failed";
        const bool artworkPlanningComplete = !catalogRefreshFailed && !cancellation->load();
        m_artworkPlanningComplete.store(artworkPlanningComplete);
        m_metadataActive.store(false);
        if (cwOk)
            pending.remoteSnapshot.continueWatching = cw;
        if (raOk)
            pending.remoteSnapshot.recentlyAdded = ra;
        if (optionalRailFailed)
            std::printf("[HomeScreen] optional_home_rail_failed catalog_population_continues\n");
        completeTelemetry(catalogRefreshFailed ? Outcome::Failure : Outcome::Success);
        // Bounded season-poster prefetch: resolve the highest-value series
        // (from the already-fetched CW/RA rails, deduplicated and capped) to
        // full catalog rows, then hand the hierarchy walk to the coordinator.
        // Home consumes immutable per-series results on the SDL thread and
        // retains the cache/artwork/progress state locally.
        if (!catalogRefreshFailed) {
            try {
                const auto seriesIds = collectBoundedSeriesIds(cw, ra);
                std::vector<MediaItem> resolvedItems;
                std::size_t resolvedCount = 0;
                if (!seriesIds.empty() && m_libraryQuery) {
                    auto resolved = m_libraryQuery->itemsByIds(seriesIds, cancellation).get();
                    if (resolved.success && !resolved.cancelled && !resolved.superseded) {
                        resolvedItems = std::move(resolved.items);
                    }
                    for (const auto& item : resolvedItems)
                        if (!item.id.empty() && item.type == "show")
                            ++resolvedCount;
                }
                std::printf("[HomeScreen] season prefetch: %zu candidates, %zu resolved\n",
                            seriesIds.size(), resolvedCount);
                if (!cancellation->load() && m_libraryCoordinator)
                    (void)requestHierarchy(resolvedItems, committedCatalogGeneration(),
                                           forceHierarchyReconcile);
            } catch (...) {
                std::printf("[HomeScreen] season prefetch skipped: exception\n");
            }
        }
        // Image-cache janitor: prune oldest files if over disk cap.
        // Runs on the fetch thread (background worker), never on the
        // UI/SDL thread.
        try {
            ImageCache::runJanitor();
        } catch (...) {
            std::printf("[HomeScreen] janitor skipped: exception\n");
        }
        pending.cacheSaved = cacheSaved;
        pending.complete = true;
        pending.catalogCommitted = m_fetchCatalogCommitted.load();
        pending.contentValid = !catalogRefreshFailed || pending.catalogCommitted;
        pending.libraryOffline = false;
        if (pending.diagnosticStage.empty())
            pending.diagnosticStage =
                catalogRefreshFailed ? "terminal_failure" : "terminal_success";
        if (pending.diagnosticGeneration == 0)
            pending.diagnosticGeneration = committedCatalogGeneration();
        publish();
        m_fetchDone.store(true);
        m_fetchDone = true;
    });
    return true;
}
void HomeScreen::requestFetch(Uint32 now)
{
    if (m_syncSchedule.request(now))
        startFetch();
}
} // namespace miyoofin
