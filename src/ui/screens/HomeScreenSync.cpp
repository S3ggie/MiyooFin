#include "HomeScreen.hpp"
#include "../../library/OfflineLibraryQuery.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include "../ArtworkLayout.hpp"
#include "../HomeSyncState.hpp"
#include <chrono>
#include <cstdio>
#include <ctime>

namespace miyoofin {

static constexpr std::int64_t HIERARCHY_RECONCILE_MS=24LL*60*60*1000;
static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}
static bool supportedLibraryView(const LibraryView &view)
{ return view.collectionType == "movies" || view.collectionType == "tvshows"; }

void HomeScreen::requestMediaPage(MediaPageState &state)
{
    if (state.inFlight || !state.hasMore)
        return;
    state.cancellation = std::make_shared<std::atomic_bool>(false);
    if (presentationOffline()) {
        std::promise<library::MediaPage> page;
        page.set_value(offlineMediaPage(m_offlineSnapshot, state.type,
                                        state.letter, state.type == "anime" ? 64 : 24,
                                        state.next));
        state.future = page.get_future();
        state.inFlight = true;
        return;
    }
    if (!m_libraryQuery)
        return;
    if (state.type == "movie")
        state.future = m_libraryQuery->movies(state.letter, 24, state.next);
    else if (state.type == "anime")
        state.future = m_libraryQuery->anime(state.letter, 64, state.next);
    else
        state.future = m_libraryQuery->shows(state.letter, 24, state.next);
    state.inFlight = true;
    if (!m_firstMediaPageReadLogged) {
        m_firstMediaPageReadLogged = true;
        uiDiagnostics().log(
            "[HomeScreen] startup stage=first_read_media_page_requested");
    }
}

void HomeScreen::requestEarlierMediaPage(MediaPageState &state)
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
        const auto &rows = m_tabs[tabIndex("Movies")].rows;
        const auto &items = rows.empty() ? m_movieWindow : rows[0].items;
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
    if (!m_librarySync || presentationOffline()) return;
    if (m_liveChangeThread.joinable()) {
        if (m_liveChangeDone.load()) finishLiveChangeApply();
        return;
    }
    // Do not begin a competing top-level sync while the initial
    // population's top-level generation is still in flight.  Events
    // remain queued and will be processed once the initial population
    // commits.
    if (m_initialPopulationInProgress)
        return;
    JellyfinLibraryChangeBatch batch;
    if (m_librarySync->takeLiveChange(batch))
        startLiveChangeApply(batch);
}

void HomeScreen::startLiveChangeApply(const JellyfinLibraryChangeBatch &batch)
{
    if (m_liveChangeThread.joinable()) return;
    m_liveChangeBatch = batch;
    m_liveChangeResult = {};
    m_liveChangeDone.store(false);
    m_liveChangeInFlight = true;
    m_liveChangeCancellation = std::make_shared<std::atomic_bool>(false);
    const auto sync = m_librarySync;
    const auto cancellation = m_liveChangeCancellation;
    const std::int64_t checkpointMs = m_syncState.lastSuccessfulMs;
    m_liveChangeThread = std::thread(
        [this, sync, batch, cancellation, checkpointMs] {
            library::LiveLibraryChangeResult result;
            if (batch.catchUpRequired) {
                if (checkpointMs > 0) {
                    const auto catchUp = sync->catchUpChangedCatalog(
                        checkpointMs, cancellation).get();
                    if (!catchUp.success) {
                        result.cancelled = catchUp.cancelled;
                        result.superseded = catchUp.superseded;
                        result.error = catchUp.error;
                        result.message = catchUp.message;
                        result.catchUpRequired = !catchUp.cancelled
                            && !catchUp.superseded;
                        m_liveChangeResult = std::move(result);
                        m_liveChangeDone.store(true);
                        return;
                    }
                }
                const auto reconciled = sync->reconcileAuthoritativeMembership(
                    cancellation).get();
                if (!reconciled.success) {
                    result.cancelled = reconciled.cancelled;
                    result.superseded = reconciled.superseded;
                    result.error = reconciled.error;
                    result.message = reconciled.message;
                    result.catchUpRequired = !reconciled.cancelled
                        && !reconciled.superseded;
                    m_liveChangeResult = std::move(result);
                    m_liveChangeDone.store(true);
                    return;
                }
            }
            result = sync->applyLibraryChanges(batch, cancellation).get();
            m_liveChangeResult = std::move(result);
            m_liveChangeDone.store(true);
        });
}

void HomeScreen::startHomeRailRefresh()
{
    if (m_homeRailRefreshInFlight) return;
    if (m_homeRailRefreshThread.joinable())
        m_homeRailRefreshThread.join();
    m_homeRailRefreshDone.store(false);
    m_homeRailRefreshInFlight = true;
    m_homeRailRefreshSucceeded = false;
    m_homeRailContinueValid = false;
    m_homeRailRecentValid = false;
    m_homeRailContinueWatching.clear();
    m_homeRailRecentlyAdded.clear();
    m_homeRailRefreshError.clear();
    m_homeRailRefreshCancellation = std::make_shared<std::atomic_bool>(false);
    const Session session = m_session;
    const auto cancellation = m_homeRailRefreshCancellation;
    m_homeRailRefreshThread = std::thread(
        [this, session, cancellation] {
            std::vector<MediaItem> continueWatching;
            std::vector<MediaItem> recentlyAdded;
            std::string continueError;
            std::string recentError;
            const bool continueOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getResumeItems(
                        base, session.accessToken, session.userId,
                        session.deviceId, 12, continueWatching, continueError,
                        cancellation.get());
                }, continueError);
            const bool recentOk = RouteRequest(session).run(
                [&](const std::string &base) {
                    return JellyfinApi::getLatestItems(
                        base, session.accessToken, session.userId,
                        session.deviceId, 16, recentlyAdded, recentError,
                        cancellation.get());
                }, recentError);
            if (continueOk) {
                m_homeRailContinueWatching = std::move(continueWatching);
                m_homeRailContinueValid = true;
            }
            if (recentOk) {
                m_homeRailRecentlyAdded = std::move(recentlyAdded);
                m_homeRailRecentValid = true;
            }
            m_homeRailRefreshSucceeded = continueOk || recentOk;
            if (!continueOk) m_homeRailRefreshError = continueError;
            else if (!recentOk) m_homeRailRefreshError = recentError;
            m_homeRailRefreshDone.store(true);
        });
}

void HomeScreen::startSafetyReconcile()
{
    if (m_safetyReconcileInFlight || !m_librarySync || presentationOffline())
        return;
    // Do not begin a competing top-level sync while the initial
    // population's top-level generation is still in flight.
    if (m_initialPopulationInProgress)
        return;
    if (m_safetyReconcileThread.joinable())
        m_safetyReconcileThread.join();
    m_safetyReconcileDone.store(false);
    m_safetyReconcileInFlight = true;
    m_safetyReconcileError.clear();
    m_safetyReconcileCancellation = std::make_shared<std::atomic_bool>(false);
    const auto sync = m_librarySync;
    const auto cancellation = m_safetyReconcileCancellation;
    const std::int64_t checkpointMs = m_syncState.lastSuccessfulMs;
    m_safetyReconcileThread = std::thread(
        [this, sync, cancellation, checkpointMs] {
            if (checkpointMs > 0) {
                const auto catchUp = sync->catchUpChangedCatalog(
                    checkpointMs, cancellation).get();
                if (!catchUp.success) {
                    m_safetyReconcileError = catchUp.message;
                    m_safetyReconcileDone.store(true);
                    return;
                }
            }
            const auto result = sync->reconcileAuthoritativeMembership(
                cancellation).get();
            if (!result.success) m_safetyReconcileError = result.message;
            m_safetyReconcileDone.store(true);
        });
}

void HomeScreen::startFetch()
{
    if (m_fetchThread.joinable()) return;
    m_metadataCompleted.store(0);
    m_metadataTotal.store(0);
    m_metadataActive.store(true);
    m_artworkPlanningComplete.store(false);
    m_fetchDone = false; m_fetchReady.store(false); m_fetchComplete.store(false);
    m_fetchPublished = false; m_fetchError.clear(); m_fetchResult.clear();
    m_fetchCacheSaved = false; m_fetchOfflinePrepared = false;
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        m_animeItemIds.clear();
    }
    m_fetchCancellation = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> cancellation = m_fetchCancellation;
    Session session=m_session; std::string url=session.serverUrl; std::string token=m_session.accessToken; std::string uid=m_session.userId; std::string devId=m_session.deviceId;
    m_fetchThread = std::thread([this, session, url, token, uid, devId, cancellation]() {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, true);
        telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
        const std::string scope=LibraryCache::scopeKey(url,uid);
        CatalogDbJobMetadata metadata=m_catalogMetadata;
        metadata.cancellation=cancellation;
        SyncState legacyState;
        const bool legacyAvailable=SyncStateStore::load(
            SyncStateStore::path("cache",scope),legacyState,nullptr);
        // Online Home readiness must not wait on CatalogDb, even for the
        // small sync checkpoint.  The checkpoint is advisory here; the
        // bounded Home reads below are the only startup data dependency.
        // Background sync/reconcile paths can refresh these fields later.
        CatalogDbSyncState catalogState;
        if (legacyAvailable) {
            m_syncState=legacyState;
            catalogState.success=true;
            catalogState.lastSuccessfulMs=legacyState.lastSuccessfulMs;
            catalogState.lastReconcileMs=legacyState.lastReconcileMs;
        }
        m_forceHierarchyReconcile=!catalogState.success
            || !syncStateFresh(m_syncState,wallClockMs(),HIERARCHY_RECONCILE_MS);
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
                for (const auto &batch :
                     OfflineLibraryQuery::metadataBatches(downloads)) {
                    if (cancellation->load()) return;
                    const auto metadataPage = m_libraryQuery->itemsByIds(
                        batch, cancellation).get();
                    if (metadataPage.cancelled || metadataPage.superseded)
                        return;
                    if (metadataPage.success) {
                        metadataItems.insert(metadataItems.end(),
                                              metadataPage.items.begin(),
                                              metadataPage.items.end());
                    }
                }
            }
            m_cachedSnapshot = OfflineLibraryQuery::build(
                downloads, metadataItems);
            m_haveCachedSnapshot = true;
            m_fetchResult = offlineTabsFromSnapshot(m_cachedSnapshot);
            m_remoteSnapshot = m_cachedSnapshot;
            m_fetchCacheSaved = true;
            m_metadataActive.store(false);
            completeTelemetry(Outcome::Success);
            m_fetchComplete.store(true);
            m_fetchReady.store(true);
            m_fetchDone = true;
            return;
        }
        bool optionalRailFailed = false;
        bool catalogRefreshFailed = false;
        bool initialPagePublished = false;
        bool coldFirstPagePublished = false;
        bool firstBoundedRequestLogged = false;
        bool firstPagePersistedLogged = false;
        uiDiagnostics().log("[HomeScreen] startup stage=home_fetch_started");
        // Probe the committed catalog before starting any network refresh. These
        // are bounded reads on the CatalogDb worker; the SDL thread only sees
        // the publication signal in update(). A genuinely empty catalog must
        // remain Loading until the authoritative network generation commits.
        if (m_catalogDb && m_catalogMetadata.scopeEpoch != 0) {
            auto warmMovies = m_catalogDb->readMediaPage(
                "movie", -1, 24, {}, metadata);
            auto warmShows = m_catalogDb->readMediaPage(
                "show", -1, 24, {}, metadata);
            const auto movies = warmMovies.get();
            const auto shows = warmShows.get();
            if (!movies.items.empty() || !shows.items.empty()) {
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
                    for (const auto &item : shows.items) {
                        const auto found = shows.membershipsByItem.find(item.id);
                        if (found == shows.membershipsByItem.end()) continue;
                        for (const auto &membership : found->second) {
                            if (isAnimeSeries(membership.viewName, item)) {
                                m_animeItemIds.insert(item.id);
                                break;
                            }
                        }
                    }
                    m_fetchResult = std::move(warmTabs);
                }
                uiDiagnostics().log(
                    "[HomeScreen] startup stage=warm_sqlite_catalog_ready");
                initialPagePublished = true;
                m_fetchReady.store(true);
            }
        }
        std::vector<MediaItem> cw; std::string cwErr;
        // Home rails are ephemeral presentation data. They are refreshed from
        // Jellyfin and are deliberately excluded from CatalogDb persistence.
        uiDiagnostics().log("[HomeScreen] startup stage=continue_watching_started");
        m_initialPopulationInProgress = true;
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12, cw, cwErr, cancellation.get());},cwErr)) { optionalRailFailed=true; printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str()); }
        uiDiagnostics().log("[HomeScreen] startup stage=continue_watching_finished");
        std::vector<MediaItem> ra; std::string raErr;
        uiDiagnostics().log("[HomeScreen] startup stage=recently_added_started");
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLatestItems(base, token, uid, devId, 16, ra, raErr, cancellation.get());},raErr)) { optionalRailFailed=true; printf("[HomeScreen] Recently added: %s\n", raErr.c_str()); }
        uiDiagnostics().log("[HomeScreen] startup stage=recently_added_finished");
        queuePosterJobs(planHomeRailPosterJobs(cw, ra), true);
        std::string viewsErr;
        uiDiagnostics().log("[HomeScreen] startup stage=views_started");
        const std::uint64_t syncGeneration = ++m_topLevelSyncGeneration;
        bool topLevelSyncStarted = false;
        // Accumulate fetched items per collection type so the
        // post-finalize rebuild can populate Home tab items.
        std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
        std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
        if (m_librarySync) {
            auto begin = m_librarySync->begin(syncGeneration).get();
            topLevelSyncStarted = begin.success;
            if (!topLevelSyncStarted) catalogRefreshFailed = true;
        } else {
            catalogRefreshFailed = true;
        }
        if (!RouteRequest(session).run([&](const std::string &base){
                return JellyfinApi::getViews(base, token, uid, devId, views,
                                             viewsErr, cancellation.get());
        }, viewsErr)) {
            catalogRefreshFailed = true;
            m_fetchError = viewsErr.empty() ? "Failed to fetch libraries" : viewsErr;
        } else {
            uiDiagnostics().log("[HomeScreen] startup stage=views_finished");
            std::printf("[HomeScreen] population_coordinator views=%zu\n", views.size());
            views.erase(std::remove_if(views.begin(), views.end(), [](const LibraryView &view) {
                if (supportedLibraryView(view)) return false;
                std::printf("[HomeScreen] library_view_skipped unsupported_collection=%s\n",
                            view.collectionType.c_str());
                return true;
            }), views.end());
            if (views.empty()) {
                std::lock_guard<std::mutex> lock(m_fetchMutex);
                m_fetchResult = JellyfinApi::buildTabs(views,cw,ra,{},{ });
                m_fetchReady.store(true);
            }
            std::size_t metadataTotal = 0;
            for (const auto &view : views) {
                const std::string types = view.collectionType == "tvshows" ? "Series" : "Movie";
                int start = 0;
                bool firstPageForView = true;
                for (;;) {
                    if (cancellation->load()) { catalogRefreshFailed = true; break; }
                    LibraryItemsPage page;
                    std::string pageErr;
                    ++requestCount;
                    if (!firstBoundedRequestLogged) {
                        firstBoundedRequestLogged = true;
                        uiDiagnostics().log(
                            "[HomeScreen] startup stage=first_bounded_request_started");
                    }
                    if (!RouteRequest(session).run([&](const std::string &base){
                            return JellyfinApi::getLibraryItemsPage(
                                base, token, uid, devId, view.id, types, start, 48,
                                page, pageErr, cancellation.get());
                        }, pageErr)) {
                        catalogRefreshFailed = true;
                        m_fetchError = pageErr.empty() ? "Failed to fetch library page" : pageErr;
                        break;
                    }
                    mediaCount += static_cast<uint32_t>(page.items.size());
                    if (firstPageForView) {
                        metadataTotal += page.totalRecordCount > 0
                            ? static_cast<std::size_t>(page.totalRecordCount)
                            : page.items.size();
                        m_metadataTotal.store(metadataTotal);
                        firstPageForView = false;
                    }
                    m_metadataCompleted.fetch_add(page.items.size());
                    if (view.collectionType == "tvshows") {
                        std::lock_guard<std::mutex> lock(m_fetchMutex);
                        for (const auto &item : page.items)
                            if (isAnimeSeries(view.name, item))
                                m_animeItemIds.insert(item.id);
                    }
                    std::printf("[HomeScreen] page_validated start=%d count=%zu more=%d\n",
                                page.startIndex, page.items.size(), page.hasMore ? 1 : 0);
                    queuePosterJobs(planMediaPagePosterJobs(page.items), start == 0);
                    if (m_catalogDb) {
                        CatalogDbMediaPageWrite writePage;
                        writePage.items = page.items;
                        writePage.viewId = view.id;
                        writePage.viewName = view.name;
                        writePage.collectionType = view.collectionType;
                        writePage.ordinalStart = static_cast<std::size_t>(start);
                        writePage.viewOrdinal = static_cast<int>(&view - views.data());
                        writePage.syncGeneration = syncGeneration;
                        writePage.finalPage = !page.hasMore;
                        auto write = m_librarySync->stage(writePage);
                        const CatalogDbMediaPageUpsertResult writeResult = write.get();
                    if (!writeResult.success) {
                            catalogRefreshFailed = true;
                            m_fetchError = writeResult.message.empty()
                                ? "Failed to persist library page" : writeResult.message;
                            std::printf("[HomeScreen] page_write_failed error=%u cancelled=%d superseded=%d message=%s\n",
                                        static_cast<unsigned>(writeResult.error),
                                        writeResult.cancelled ? 1 : 0,
                                        writeResult.superseded ? 1 : 0,
                                        writeResult.message.c_str());
                            break;
                        }
                    } else {
                        catalogRefreshFailed = true;
                        std::printf("[HomeScreen] page_write_failed error=db_unavailable\n");
                        break;
                    }
                    // Accumulate fetched items for post-finalize tab rebuild.
                    // Bound to 24 items per view to match the warm path's
                    // readMediaPage(-1, 24) and prevent unbounded memory use.
                    {
                        static constexpr std::size_t kColdViewLimit = 24;
                        auto &targetList = (view.collectionType == "tvshows")
                            ? showsByView : moviesByView;
                        if (targetList.empty()
                            || targetList.back().first != view.name)
                            targetList.emplace_back(view.name,
                                std::vector<MediaItem>());
                        auto &items = targetList.back().second;
                        if (items.size() < kColdViewLimit) {
                            const std::size_t space =
                                kColdViewLimit - items.size();
                            const std::size_t toAdd =
                                std::min(space, page.items.size());
                            items.insert(items.end(), page.items.begin(),
                                page.items.begin()
                                + static_cast<std::ptrdiff_t>(toAdd));
                        }
                    }
                    if (!firstPagePersistedLogged) {
                        firstPagePersistedLogged = true;
                        uiDiagnostics().log(
                            "[HomeScreen] startup stage=first_page_persisted");
                    }
                    if (!initialPagePublished) {
                        // Home becomes useful after the first bounded page;
                        // remaining pages continue in this worker and are
                        // available to lazy CatalogDb reads as they commit.
                        {
                            std::lock_guard<std::mutex> lock(m_fetchMutex);
                            m_fetchResult=JellyfinApi::buildTabs(views,cw,ra,{},{});
                        }
                        std::printf("[HomeScreen] first bounded page ready views=%zu\n",
                                    views.size());
                        initialPagePublished = true;
                        coldFirstPagePublished = true;
                        uiDiagnostics().log("[HomeScreen] startup stage=first_bounded_page_ready");
                        m_fetchReady.store(true);
                    }
                    if (!page.hasMore || page.items.empty()) break;
                    start = page.startIndex + static_cast<int>(page.items.size());
                }
                if (catalogRefreshFailed) break;
            }
        }
        if (topLevelSyncStarted && !catalogRefreshFailed && !cancellation->load()) {
            auto finalized = m_librarySync->finalize(syncGeneration).get();
            if (!finalized.success) {
                catalogRefreshFailed = true;
            } else if (coldFirstPagePublished) {
                // The initial first-bounded-page publish used empty
                // movie/show lists.  Rebuild with the actually-fetched
                // items so Home tabs render populated on cold start.
                {
                    std::lock_guard<std::mutex> lock(m_fetchMutex);
                    m_fetchResult = JellyfinApi::buildTabs(
                        views, cw, ra, moviesByView, showsByView);
                }
            }
        } else if (topLevelSyncStarted) {
            m_librarySync->abort(syncGeneration).get();
        }
        m_initialPopulationInProgress = false;
        if (catalogRefreshFailed && m_fetchError.empty())
            m_fetchError = "Library refresh failed";
        const bool artworkPlanningComplete =
            !catalogRefreshFailed && !cancellation->load();
        m_artworkPlanningComplete.store(artworkPlanningComplete);
        m_metadataActive.store(false);
        m_remoteSnapshot.continueWatching=cw;
        m_remoteSnapshot.recentlyAdded=ra;
        if (optionalRailFailed)
            std::printf("[HomeScreen] optional_home_rail_failed catalog_population_continues\n");
        completeTelemetry(catalogRefreshFailed ? Outcome::Failure : Outcome::Success);
        m_fetchComplete.store(true);
        m_fetchDone.store(true);
        m_fetchReady.store(true);
        m_fetchDone=true;
    });
}
void HomeScreen::requestFetch(Uint32 now){if(m_syncSchedule.request(now))startFetch();}
}
