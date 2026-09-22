#include "HomeLibraryController.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../library/OfflineLibraryQuery.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../ShowsBrowser.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace miyoofin {

HomeLibraryController::HomeLibraryController(const Session& session, library::LibraryQuery* query,
                                             library::LibraryCoordinator* coordinator,
                                             DownloadManager* downloads)
    : m_session(session), m_libraryQuery(query), m_libraryCoordinator(coordinator),
      m_downloads(downloads)
{}

HomeLibraryController::~HomeLibraryController()
{
    requestStopAllWorkers();
    joinAllWorkers();
}

std::uint64_t HomeLibraryController::catalogScopeEpoch() const
{
    return m_libraryQuery ? m_libraryQuery->scopeEpoch() : 0;
}

std::uint64_t HomeLibraryController::committedCatalogGeneration() const
{
    return m_libraryCoordinator ? m_libraryCoordinator->status().committedGeneration : 0;
}

HomeLibraryController::OfflineSnapshotSignature
HomeLibraryController::computeOfflineSignature(const DownloadSnapshot& downloads,
                                               std::uint64_t catalogGeneration)
{
    OfflineSnapshotSignature signature;
    signature.localBytes = downloads.localBytes;
    signature.reservedBytes = downloads.reservedBytes;
    signature.catalogGeneration = catalogGeneration;
    for (const auto& item : downloads.items) {
        if (OfflineLibraryQuery::isAvailable(item.state)) {
            signature.availableItemIds.insert(item.itemId);
            signature.totalDownloadedBytes += item.downloadedBytes;
            ++signature.availableItemCount;
        }
    }
    return signature;
}

void HomeLibraryController::addArtwork(Presentation& presentation, std::vector<HomePosterJob> jobs,
                                       bool highPriority)
{
    if (!jobs.empty())
        presentation.artwork.push_back({std::move(jobs), highPriority});
}

void HomeLibraryController::publish(Presentation presentation)
{
    if (presentation.fetchGeneration != m_fetchGeneration.load(std::memory_order_acquire))
        return;
    const bool complete = presentation.complete;
    const std::string diagnostic =
        "[HomeScreen] pending_presentation_published request=" +
        std::to_string(presentation.diagnosticRequest) +
        " generation=" + std::to_string(presentation.diagnosticGeneration) + " stage=" +
        (presentation.diagnosticStage.empty() ? "unspecified" : presentation.diagnosticStage) +
        " complete=" + std::to_string(complete ? 1 : 0) +
        " content=" + std::to_string(presentation.contentValid ? 1 : 0) +
        " error=" + std::to_string(presentation.error.empty() ? 0 : 1);
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        m_pendingPresentation = std::make_shared<const Presentation>(std::move(presentation));
        m_fetchComplete.store(complete);
        m_fetchReady.store(true);
    }
    uiDiagnostics().log(diagnostic);
}

bool HomeLibraryController::takePresentation(Presentation& presentation)
{
    std::lock_guard<std::mutex> lock(m_fetchMutex);
    if (!m_pendingPresentation)
        return false;
    if (m_pendingPresentation->fetchGeneration !=
        m_fetchGeneration.load(std::memory_order_acquire)) {
        m_pendingPresentation.reset();
        m_fetchReady.store(false);
        return false;
    }
    presentation = *m_pendingPresentation;
    m_pendingPresentation.reset();
    m_fetchReady.store(false);
    return true;
}

bool HomeLibraryController::requestHomeRailRefresh()
{
    std::lock_guard<std::mutex> lock(m_railMutex);
    if (m_homeRailInFlight) {
        return false;
    }
    if (!m_libraryCoordinator)
        return false;
    std::uint64_t request = 0;
    if (!m_libraryCoordinator->requestHomeRailRefresh(request))
        return false;
    m_homeRailRequest = request;
    m_homeRailInFlight = true;
    return true;
}

bool HomeLibraryController::takeHomeRailRefresh(RailPresentation& result)
{
    std::lock_guard<std::mutex> lock(m_railMutex);
    if (!m_homeRailInFlight || !m_libraryCoordinator)
        return false;
    library::HomeRailResult rail;
    if (!m_libraryCoordinator->takeHomeRailResult(m_homeRailRequest, rail) ||
        rail.request != m_homeRailRequest)
        return false;
    result.request = rail.request;
    result.success = rail.success;
    result.continueValid = rail.continueValid;
    result.recentlyAddedValid = rail.recentlyAddedValid;
    result.continueWatching = std::move(rail.continueWatching);
    result.recentlyAdded = std::move(rail.recentlyAdded);
    result.error = std::move(rail.error);
    m_homeRailInFlight = false;
    return true;
}

void HomeLibraryController::requestStopAllWorkers() noexcept
{
    m_stopRequested.store(true);
    cancelFetch();
}

void HomeLibraryController::cancelFetch() noexcept
{
    if (m_fetchCancellation)
        m_fetchCancellation->store(true);
    if (m_libraryCoordinator) {
        m_libraryCoordinator->cancelStartupSync();
        m_libraryCoordinator->cancelFullPopulation();
        m_libraryCoordinator->cancelHomeRailRefresh();
        m_libraryCoordinator->cancelHierarchy();
    }
}

void HomeLibraryController::joinAllWorkers()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
}

bool HomeLibraryController::startFetch(const std::vector<TabData>& previousTabs,
                                       const LibrarySnapshot& previousCachedSnapshot,
                                       const LibrarySnapshot& previousRemoteSnapshot,
                                       bool previousHaveCachedSnapshot, bool previousLibraryOffline,
                                       bool previousContentValid,
                                       const std::set<std::string>& previousAnimeItemIds)
{
    if (m_stopRequested.load())
        return false;
    if (m_fetchThread.joinable()) {
        if (m_fetchComplete.load() || m_fetchDone.load())
            m_fetchThread.join();
        else
            return false;
    }
    m_previousTabs = previousTabs;
    m_previousCachedSnapshot = previousCachedSnapshot;
    m_previousRemoteSnapshot = previousRemoteSnapshot;
    m_previousHaveCachedSnapshot = previousHaveCachedSnapshot;
    m_previousLibraryOffline = previousLibraryOffline;
    m_previousContentValid = previousContentValid;
    m_previousAnimeItemIds = previousAnimeItemIds;
    m_metadataCompleted.store(0);
    m_metadataTotal.store(0);
    m_metadataActive.store(true);
    m_artworkPlanningComplete.store(false);
    m_fetchDone.store(false);
    m_fetchReady.store(false);
    m_fetchComplete.store(false);
    m_fetchCatalogCommitted.store(false);
    m_initialPopulationInProgress.store(false);
    m_fetchCancellation = std::make_shared<std::atomic<bool>>(false);
    const auto cancellation = m_fetchCancellation;
    const Session session = m_session;
    const auto fetchGeneration = m_fetchGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    m_fetchThread = std::thread(&HomeLibraryController::fetchWorker, this, session, fetchGeneration,
                                cancellation);
    return true;
}

void HomeLibraryController::fetchWorker(Session session, std::uint64_t fetchGeneration,
                                        std::shared_ptr<std::atomic<bool>> cancellation)
{
    PerformanceTelemetry& telemetry = performanceTelemetry();
    telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, true);
    telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
    Presentation pending;
    pending.fetchGeneration = fetchGeneration;
    pending.previousTabs = m_previousTabs;
    pending.previousCachedSnapshot = m_previousCachedSnapshot;
    pending.previousRemoteSnapshot = m_previousRemoteSnapshot;
    pending.previousHaveCachedSnapshot = m_previousHaveCachedSnapshot;
    pending.previousLibraryOffline = m_previousLibraryOffline;
    pending.previousContentValid = m_previousContentValid;
    pending.previousAnimeItemIds = m_previousAnimeItemIds;
    auto publishPending = [&]() { publish(pending); };
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
        auto finishCancelledOfflineFetch = [&]() {
            pending.error = "Library refresh cancelled";
            pending.complete = true;
            pending.cancelled = true;
            pending.diagnosticStage = "offline_terminal";
            publishPending();
            m_metadataActive.store(false);
            completeTelemetry(Outcome::Cancelled);
            m_fetchDone.store(true);
        };
        const DownloadSnapshot downloads =
            m_downloads ? m_downloads->snapshot() : DownloadSnapshot{};
        std::vector<MediaItem> metadataItems;
        if (m_libraryQuery) {
            for (const auto& batch : OfflineLibraryQuery::metadataBatches(downloads)) {
                if (cancellation->load()) {
                    finishCancelledOfflineFetch();
                    return;
                }
                const auto metadataPage = m_libraryQuery->itemsByIds(batch, cancellation).get();
                if (metadataPage.cancelled || metadataPage.superseded || cancellation->load()) {
                    finishCancelledOfflineFetch();
                    return;
                }
                if (metadataPage.success)
                    metadataItems.insert(metadataItems.end(), metadataPage.items.begin(),
                                         metadataPage.items.end());
            }
        }
        if (cancellation->load()) {
            finishCancelledOfflineFetch();
            return;
        }
        pending.cachedSnapshot = OfflineLibraryQuery::build(downloads, metadataItems);
        pending.haveCachedSnapshot = true;
        pending.libraryOffline = true;
        pending.offlineCacheValid = true;
        pending.offlineSignature = computeOfflineSignature(downloads, committedCatalogGeneration());
        OfflineCatalogSnapshot catalog;
        OfflineLibraryProjection projection(pending.cachedSnapshot, catalog, downloads);
        pending.preparedOfflineSnapshot = pending.cachedSnapshot;
        const std::set<std::string> movieIds = [&] {
            std::set<std::string> ids;
            for (const auto& item : projection.movies())
                ids.insert(item.id);
            return ids;
        }();
        for (auto& view : pending.preparedOfflineSnapshot.movies) {
            view.items.erase(
                std::remove_if(view.items.begin(), view.items.end(),
                               [&](const MediaItem& item) { return !movieIds.count(item.id); }),
                view.items.end());
        }
        for (auto& view : pending.preparedOfflineSnapshot.shows) {
            view.items.erase(std::remove_if(view.items.begin(), view.items.end(),
                                            [&](const MediaItem& item) {
                                                return !projection.playable(item.id) &&
                                                       projection.seasons(item.id).empty();
                                            }),
                             view.items.end());
        }
        pending.offlineTabs = offlineTabsFromSnapshot(pending.preparedOfflineSnapshot);
        for (auto& tab : pending.offlineTabs) {
            if (tab.name == "Movies")
                tab.rows = {{"Movies", {}}};
            if (tab.name == "Shows")
                tab.rows = {{"Shows", {}}};
        }
        pending.offlinePrepared = true;
        pending.offlineSnapshotCache = pending.preparedOfflineSnapshot;
        pending.tabs = pending.offlineTabs;
        pending.remoteSnapshot = pending.cachedSnapshot;
        pending.cacheSaved = true;
        pending.contentValid = true;
        pending.continueValid = true;
        pending.recentlyAddedValid = true;
        pending.continueWatching = pending.cachedSnapshot.continueWatching;
        pending.recentlyAdded = pending.cachedSnapshot.recentlyAdded;
        pending.complete = true;
        pending.diagnosticStage = "offline_terminal";
        publishPending();
        m_metadataActive.store(false);
        completeTelemetry(Outcome::Success);
        m_fetchDone.store(true);
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
            warmTabs.push_back({"Movies", {{"Movies", movies.items}}});
            warmTabs.push_back({"Shows", {{"Shows", shows.items}}});
            warmTabs.push_back({"Downloads", {{"", {}}}});
            warmTabs.push_back({"Settings", {{"", {}}}});
            for (const auto& item : shows.items) {
                const auto found = shows.membershipsByItem.find(item.id);
                if (found == shows.membershipsByItem.end())
                    continue;
                for (const auto& membership : found->second)
                    if (isAnimeSeries(membership.viewName, item)) {
                        pending.animeItemIds.insert(item.id);
                        break;
                    }
            }
            pending.tabs = std::move(warmTabs);
            pending.contentValid = true;
            pending.stale = true;
            publishPending();
            uiDiagnostics().log("[HomeScreen] startup stage=warm_sqlite_catalog_ready");
            initialPagePublished = true;
        }
    }
    m_initialPopulationInProgress.store(true);
    if (m_libraryCoordinator)
        coordinatorStartupStarted = m_libraryCoordinator->startStartupSync(initialPagePublished);

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
    addArtwork(pending, planHomeRailPosterJobs(cw, ra), true);
    publishPending();
    if (coordinatorStartupStarted) {
        for (;;) {
            if (cancellation->load())
                m_libraryCoordinator->cancelStartupSync();
            if (m_libraryCoordinator->takeStartupSyncResult(startupSyncResult))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    } else {
        startupSyncResult.mode = library::StartupSyncMode::FullReconcile;
    }

    std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
    std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
    bool forceHierarchyReconcile =
        startupSyncResult.mode == library::StartupSyncMode::FullReconcile;
    try {
        bool deltaCatchUpSucceeded = false;
        if (startupSyncResult.mode == library::StartupSyncMode::SkipFresh) {
            if (cwOk)
                pending.remoteSnapshot.continueWatching = cw;
            if (raOk)
                pending.remoteSnapshot.recentlyAdded = ra;
        } else if (startupSyncResult.mode == library::StartupSyncMode::DeltaCatchUp) {
            uiDiagnostics().log("[HomeScreen] startup stage=delta_catchup_started");
            if (startupSyncResult.success && !startupSyncResult.cancelled &&
                !startupSyncResult.superseded) {
                if (cwOk)
                    pending.remoteSnapshot.continueWatching = cw;
                if (raOk)
                    pending.remoteSnapshot.recentlyAdded = ra;
                deltaCatchUpSucceeded = true;
            } else if (startupSyncResult.cancelled || startupSyncResult.superseded) {
                if (cwOk)
                    pending.remoteSnapshot.continueWatching = cw;
                if (raOk)
                    pending.remoteSnapshot.recentlyAdded = ra;
                deltaCatchUpSucceeded = true;
            } else {
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
                        if (update.request != populationRequest)
                            uiDiagnostics().log(
                                "[HomeScreen] full_population_consumer identity_mismatch"
                                " requested_request=" +
                                std::to_string(populationRequest) +
                                " current_request=" + std::to_string(update.request) +
                                " current_update_generation=" + std::to_string(update.generation) +
                                " miss_count=" + std::to_string(populationMissCount));
                        if (consumerStage != populationConsumerStage) {
                            uiDiagnostics().log(
                                std::string("[HomeScreen] full_population_consumer ") +
                                (populationConsumerStage.empty() ? "hit" : "transition") +
                                " requested_request=" + std::to_string(populationRequest) +
                                " current_request=" + std::to_string(update.request) +
                                " current_update_generation=" + std::to_string(update.generation) +
                                " stage=" + consumerStage +
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
                            std::printf("[HomeScreen] page_validated start=%d count=%zu more=%d\n",
                                        page.startIndex, page.items.size(), page.hasMore ? 1 : 0);
                            addArtwork(pending, planMediaPagePosterJobs(page.items),
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
                            if (view.collectionType == "tvshows") {
                                for (const auto& item : page.items)
                                    if (isAnimeSeries(view.name, item))
                                        pending.animeItemIds.insert(item.id);
                            }
                            if (!firstPagePersistedLogged) {
                                firstPagePersistedLogged = true;
                                uiDiagnostics().log(
                                    "[HomeScreen] startup stage=first_page_persisted");
                            }
                            if (update.firstPage && !initialPagePublished) {
                                pending.tabs = buildTabs(cw, ra, moviesByView, showsByView);
                                LibrarySnapshot firstPageSnapshot;
                                firstPageSnapshot.continueWatching = cw;
                                firstPageSnapshot.recentlyAdded = ra;
                                pending.remoteSnapshot = firstPageSnapshot;
                                pending.contentValid = true;
                                pending.continueValid = cwOk;
                                pending.recentlyAddedValid = raOk;
                                pending.continueWatching = cw;
                                pending.recentlyAdded = ra;
                                publishPending();
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
                            pending.cancelled = update.cancelled || update.superseded;
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
        }
    } catch (...) {
        m_initialPopulationInProgress.store(false);
        throw;
    }
    m_initialPopulationInProgress.store(false);
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
    if (!catalogRefreshFailed) {
        try {
            const auto seriesIds = collectBoundedSeriesIds(cw, ra);
            std::vector<MediaItem> resolvedItems;
            std::size_t resolvedCount = 0;
            if (!seriesIds.empty() && m_libraryQuery) {
                auto resolved = m_libraryQuery->itemsByIds(seriesIds, cancellation).get();
                if (resolved.success && !resolved.cancelled && !resolved.superseded)
                    resolvedItems = std::move(resolved.items);
                for (const auto& item : resolvedItems)
                    if (!item.id.empty() && item.type == "show")
                        ++resolvedCount;
            }
            std::printf("[HomeScreen] season prefetch: %zu candidates, %zu resolved\n",
                        seriesIds.size(), resolvedCount);
            if (!cancellation->load() && !resolvedItems.empty()) {
                pending.hierarchyReady = true;
                pending.hierarchyShows = std::move(resolvedItems);
                pending.hierarchyGeneration = committedCatalogGeneration();
                pending.forceHierarchyReconcile = forceHierarchyReconcile;
            }
        } catch (...) {
            std::printf("[HomeScreen] season prefetch skipped: exception\n");
        }
    }
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
        pending.diagnosticStage = catalogRefreshFailed ? "terminal_failure" : "terminal_success";
    if (pending.diagnosticGeneration == 0)
        pending.diagnosticGeneration = committedCatalogGeneration();
    publishPending();
    m_fetchDone.store(true);
}

} // namespace miyoofin
