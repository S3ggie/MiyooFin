#include "HomeLibraryController.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../library/OfflineLibraryQuery.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../ShowsBrowser.hpp"
#include <algorithm>
#include <cstdio>
#include <functional>

namespace miyoofin {

namespace {

// Mutable per-fetch telemetry accumulation, emitted once when the fetch
// reaches a terminal state.  `cacheSaved` mirrors the original worker-local:
// only the online schedule path reads it and it stays false there.
struct FetchTelemetry
{
    TelemetryTimer timer;
    bool emitted = false;
    bool cacheSaved = false;
    std::uint32_t requestCount = 0;
    std::uint32_t changedHierarchyCount = 0;
    std::uint32_t mediaCount = 0;
    std::vector<LibraryView> views;
};

// Result of the best-effort optional Home rail phase.  Rail failure is
// isolated from the catalog: cached rail content is retained and population
// continues regardless.
struct HomeRailPhase
{
    bool optionalRailFailed = false;
    bool cwOk = false;
    bool raOk = false;
    std::vector<MediaItem> cw;
    std::vector<MediaItem> ra;
};

// Non-owning view of one fetch attempt.  Carries the per-fetch state and the
// owner's publication callbacks so the phase helpers stay small and free of
// long parameter lists.
struct FetchContext
{
    HomeLibraryController* owner = nullptr;
    const Session* session = nullptr;
    std::uint64_t fetchGeneration = 0;
    std::shared_ptr<std::atomic<bool>> cancellation;
    HomeLibraryController::Presentation* pending = nullptr;

    library::LibraryQuery* query = nullptr;
    library::LibraryCoordinator* coordinator = nullptr;
    DownloadManager* downloads = nullptr;

    std::atomic<bool>* metadataActive = nullptr;
    std::atomic<std::size_t>* metadataCompleted = nullptr;
    std::atomic<std::size_t>* metadataTotal = nullptr;
    std::atomic<bool>* artworkPlanningComplete = nullptr;
    std::atomic<bool>* initialPopulationInProgress = nullptr;
    std::atomic<bool>* fetchCatalogCommitted = nullptr;
    std::atomic<bool>* fetchDone = nullptr;

    FetchTelemetry* telemetry = nullptr;

    std::function<void(const HomeLibraryController::Presentation&)> publish;
    std::function<void(HomeLibraryController::Presentation&, std::vector<HomePosterJob>, bool)>
        addArtwork;
};

void completeFetchTelemetry(FetchContext& ctx, Outcome outcome) noexcept
{
    FetchTelemetry& state = *ctx.telemetry;
    if (state.emitted)
        return;
    state.emitted = true;
    PerformanceTelemetry& telemetry = performanceTelemetry();
    if (state.timer.active() && telemetry.enabledFast()) {
        TelemetryRecord record{};
        record.header.record_type = RecordType::LibrarySync;
        record.payload.library_sync.duration_us = state.timer.elapsedUs();
        record.payload.library_sync.outcome = static_cast<uint8_t>(outcome);
        record.payload.library_sync.cache_saved = state.cacheSaved ? 1 : 0;
        record.payload.library_sync.views_count = static_cast<uint32_t>(state.views.size());
        record.payload.library_sync.media_count = state.mediaCount;
        record.payload.library_sync.changed_hierarchy_count = state.changedHierarchyCount;
        record.payload.library_sync.request_count = state.requestCount;
        telemetry.emitRecord(record);
    }
    telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, false);
    telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 0);
}

// Manual-offline mode is terminal and self-contained: it builds the downloaded
// presentation and returns true so the caller stops.  Cancellation at any
// point publishes a cancelled offline terminal result and also returns true.
bool runOfflinePhase(FetchContext& ctx)
{
    HomeLibraryController::Presentation& pending = *ctx.pending;
    if (!ctx.session->manualOfflineMode)
        return false;

    auto finishCancelledOfflineFetch = [&]() {
        pending.error = "Library refresh cancelled";
        pending.complete = true;
        pending.cancelled = true;
        pending.diagnosticStage = "offline_terminal";
        ctx.publish(pending);
        ctx.metadataActive->store(false);
        completeFetchTelemetry(ctx, Outcome::Cancelled);
        ctx.fetchDone->store(true);
    };
    const DownloadSnapshot downloads =
        ctx.downloads ? ctx.downloads->snapshot() : DownloadSnapshot{};
    std::vector<MediaItem> metadataItems;
    if (ctx.query) {
        for (const auto& batch : OfflineLibraryQuery::metadataBatches(downloads)) {
            if (ctx.cancellation->load()) {
                finishCancelledOfflineFetch();
                return true;
            }
            const auto metadataPage = ctx.query->itemsByIds(batch, ctx.cancellation).get();
            if (metadataPage.cancelled || metadataPage.superseded || ctx.cancellation->load()) {
                finishCancelledOfflineFetch();
                return true;
            }
            if (metadataPage.success)
                metadataItems.insert(metadataItems.end(), metadataPage.items.begin(),
                                     metadataPage.items.end());
        }
    }
    if (ctx.cancellation->load()) {
        finishCancelledOfflineFetch();
        return true;
    }
    pending.cachedSnapshot = OfflineLibraryQuery::build(downloads, metadataItems);
    pending.haveCachedSnapshot = true;
    pending.libraryOffline = true;
    pending.offlineCacheValid = true;
    pending.offlineSignature = HomeLibraryController::computeOfflineSignature(
        downloads, ctx.owner->committedCatalogGeneration());
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
    ctx.publish(pending);
    ctx.metadataActive->store(false);
    completeFetchTelemetry(ctx, Outcome::Success);
    ctx.fetchDone->store(true);
    return true;
}

// Best-effort warm publication from the persisted SQLite catalog.  Returns
// true when a stale-but-valid page was published before the network work
// starts.
bool publishWarmCatalog(FetchContext& ctx)
{
    if (!ctx.query || ctx.owner->catalogScopeEpoch() == 0)
        return false;
    auto warmMovies = ctx.query->movies(-1, 24, {}, ctx.cancellation);
    auto warmShows = ctx.query->shows(-1, 24, {}, ctx.cancellation);
    const auto movies = warmMovies.get();
    const auto shows = warmShows.get();
    if (ctx.cancellation->load() || movies.cancelled || shows.cancelled || movies.superseded ||
        shows.superseded || (movies.items.empty() && shows.items.empty()))
        return false;
    HomeLibraryController::Presentation& pending = *ctx.pending;
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
    ctx.publish(pending);
    uiDiagnostics().log("[HomeScreen] startup stage=warm_sqlite_catalog_ready");
    return true;
}

// Optional Home rail fetch.  Rail failure is isolated: cached rail content is
// retained and the catalog population continues uninterrupted.
void fetchHomeRails(FetchContext& ctx, HomeRailPhase& rail)
{
    HomeLibraryController::Presentation& pending = *ctx.pending;
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
        ctx.coordinator && ctx.coordinator->requestHomeRailRefresh(railRequest);
    library::HomeRailResult railResult;
    if (railStarted) {
        ++ctx.telemetry->requestCount;
        if (ctx.cancellation->load())
            ctx.coordinator->cancelHomeRailRefresh();
        const auto railWait =
            ctx.coordinator->waitHomeRailResult(railRequest, railResult, ctx.cancellation.get());
        if (railWait != library::WaitStatus::Ready) {
            railResult.cancelled = railWait == library::WaitStatus::Cancelled ||
                                   railWait == library::WaitStatus::Stopped ||
                                   railWait == library::WaitStatus::Superseded;
            railResult.error = railResult.cancelled ? "Home rail refresh cancelled"
                                                    : "Home rail refresh unavailable";
        }
        if (railResult.continueValid) {
            cwOk = true;
            cw = railResult.continueWatching;
        } else {
            rail.optionalRailFailed = true;
            cwErr = railResult.error;
        }
        if (railResult.recentlyAddedValid) {
            raOk = true;
            ra = railResult.recentlyAdded;
        } else {
            rail.optionalRailFailed = true;
            raErr = railResult.error;
        }
    }
    if (!railStarted || !cwOk) {
        rail.optionalRailFailed = true;
        printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());
    }
    uiDiagnostics().log("[HomeScreen] startup stage=continue_watching_finished");
    uiDiagnostics().log("[HomeScreen] startup stage=recently_added_started");
    if (!railStarted || !raOk) {
        rail.optionalRailFailed = true;
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
    ctx.addArtwork(pending, planHomeRailPosterJobs(cw, ra), true);
    ctx.publish(pending);
    rail.cwOk = cwOk;
    rail.raOk = raOk;
    rail.cw = std::move(cw);
    rail.ra = std::move(ra);
}

// Resolve the coordinator's startup policy into a startup-sync result,
// mapping a non-ready wait into the same cancelled/unavailable shapes the
// coordinator would report.
library::StartupSyncResult waitForStartupSync(FetchContext& ctx, bool coordinatorStartupStarted)
{
    library::StartupSyncResult startupSyncResult;
    if (coordinatorStartupStarted) {
        if (ctx.cancellation->load())
            ctx.coordinator->cancelStartupSync();
        const auto startupWait =
            ctx.coordinator->waitStartupSyncResult(startupSyncResult, ctx.cancellation.get());
        if (startupWait != library::WaitStatus::Ready) {
            startupSyncResult.cancelled = startupWait == library::WaitStatus::Cancelled ||
                                          startupWait == library::WaitStatus::Stopped ||
                                          startupWait == library::WaitStatus::Superseded;
            startupSyncResult.error = startupSyncResult.cancelled
                                          ? CatalogDbErrorCategory::Superseded
                                          : CatalogDbErrorCategory::None;
            startupSyncResult.message =
                startupSyncResult.cancelled ? "startup sync cancelled" : "startup sync unavailable";
        }
    } else {
        startupSyncResult.mode = library::StartupSyncMode::FullReconcile;
    }
    return startupSyncResult;
}

// Publish the terminal follow-ups (error default, artwork/metadata state,
// optional hierarchy resolution and janitor) and the terminal Presentation,
// then signal worker completion.
void finalizeFetch(FetchContext& ctx, const HomeRailPhase& rail, bool catalogRefreshFailed,
                   bool forceHierarchyReconcile)
{
    HomeLibraryController::Presentation& pending = *ctx.pending;
    if (catalogRefreshFailed && pending.error.empty())
        pending.error = "Library refresh failed";
    const bool artworkPlanningComplete = !catalogRefreshFailed && !ctx.cancellation->load();
    ctx.artworkPlanningComplete->store(artworkPlanningComplete);
    ctx.metadataActive->store(false);
    if (rail.cwOk)
        pending.remoteSnapshot.continueWatching = rail.cw;
    if (rail.raOk)
        pending.remoteSnapshot.recentlyAdded = rail.ra;
    if (rail.optionalRailFailed)
        std::printf("[HomeScreen] optional_home_rail_failed catalog_population_continues\n");
    completeFetchTelemetry(ctx, catalogRefreshFailed ? Outcome::Failure : Outcome::Success);
    if (!catalogRefreshFailed) {
        try {
            const auto seriesIds = collectBoundedSeriesIds(rail.cw, rail.ra);
            std::vector<MediaItem> resolvedItems;
            std::size_t resolvedCount = 0;
            if (!seriesIds.empty() && ctx.query) {
                auto resolved = ctx.query->itemsByIds(seriesIds, ctx.cancellation).get();
                if (resolved.success && !resolved.cancelled && !resolved.superseded)
                    resolvedItems = std::move(resolved.items);
                for (const auto& item : resolvedItems)
                    if (!item.id.empty() && item.type == "show")
                        ++resolvedCount;
            }
            std::printf("[HomeScreen] season prefetch: %zu candidates, %zu resolved\n",
                        seriesIds.size(), resolvedCount);
            if (!ctx.cancellation->load() && !resolvedItems.empty()) {
                pending.hierarchyReady = true;
                pending.hierarchyShows = std::move(resolvedItems);
                pending.hierarchyGeneration = ctx.owner->committedCatalogGeneration();
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
    pending.cacheSaved = ctx.telemetry->cacheSaved;
    pending.complete = true;
    pending.catalogCommitted = ctx.fetchCatalogCommitted->load();
    pending.contentValid = !catalogRefreshFailed || pending.catalogCommitted;
    pending.libraryOffline = false;
    if (pending.diagnosticStage.empty())
        pending.diagnosticStage = catalogRefreshFailed ? "terminal_failure" : "terminal_success";
    if (pending.diagnosticGeneration == 0)
        pending.diagnosticGeneration = ctx.owner->committedCatalogGeneration();
    ctx.publish(pending);
    ctx.fetchDone->store(true);
}

} // namespace

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

    FetchTelemetry fetchTelemetry;
    FetchContext ctx;
    ctx.owner = this;
    ctx.session = &session;
    ctx.fetchGeneration = fetchGeneration;
    ctx.cancellation = cancellation;
    ctx.pending = &pending;
    ctx.query = m_libraryQuery;
    ctx.coordinator = m_libraryCoordinator;
    ctx.downloads = m_downloads;
    ctx.metadataActive = &m_metadataActive;
    ctx.metadataCompleted = &m_metadataCompleted;
    ctx.metadataTotal = &m_metadataTotal;
    ctx.artworkPlanningComplete = &m_artworkPlanningComplete;
    ctx.initialPopulationInProgress = &m_initialPopulationInProgress;
    ctx.fetchCatalogCommitted = &m_fetchCatalogCommitted;
    ctx.fetchDone = &m_fetchDone;
    ctx.telemetry = &fetchTelemetry;
    ctx.publish = [this](const Presentation& presentation) { publish(presentation); };
    ctx.addArtwork = [](Presentation& target, std::vector<HomePosterJob> jobs, bool highPriority) {
        addArtwork(target, std::move(jobs), highPriority);
    };
    auto& views = ctx.telemetry->views;
    auto& requestCount = ctx.telemetry->requestCount;
    auto& mediaCount = ctx.telemetry->mediaCount;

    if (runOfflinePhase(ctx))
        return;

    uiDiagnostics().log("[HomeScreen] startup stage=home_fetch_started");
    bool initialPagePublished = publishWarmCatalog(ctx);
    bool catalogRefreshFailed = false;
    bool firstBoundedRequestLogged = false;
    bool firstPagePersistedLogged = false;
    m_initialPopulationInProgress.store(true);
    bool coordinatorStartupStarted = false;
    if (m_libraryCoordinator)
        coordinatorStartupStarted = m_libraryCoordinator->startStartupSync(initialPagePublished);

    HomeRailPhase rail;
    fetchHomeRails(ctx, rail);
    const bool cwOk = rail.cwOk;
    const bool raOk = rail.raOk;
    const std::vector<MediaItem>& cw = rail.cw;
    const std::vector<MediaItem>& ra = rail.ra;
    const library::StartupSyncResult startupSyncResult =
        waitForStartupSync(ctx, coordinatorStartupStarted);

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
                        const auto populationWait = m_libraryCoordinator->waitFullPopulationUpdate(
                            populationRequest, update, cancellation.get());
                        if (populationWait != library::WaitStatus::Ready) {
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
                            populationComplete = true;
                            catalogRefreshFailed = true;
                            pending.cancelled = populationWait == library::WaitStatus::Cancelled ||
                                                populationWait == library::WaitStatus::Stopped ||
                                                populationWait == library::WaitStatus::Superseded;
                            pending.error = pending.cancelled ? "Library refresh cancelled"
                                                              : "Library refresh unavailable";
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
                            // During the initial full-population walk only the
                            // first page of each library view schedules
                            // artwork; the bounded first-page rows cover the
                            // rest.  Home rails schedule their own artwork and
                            // user-driven paging uses the LibraryQuery path, so
                            // neither is affected by this gate.
                            if (page.startIndex == 0)
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
    finalizeFetch(ctx, rail, catalogRefreshFailed, forceHierarchyReconcile);
}

} // namespace miyoofin
