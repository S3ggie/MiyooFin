#include "HomeScreen.hpp"
#include "../../cache/OfflineLibraryProjection.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/UiDiagnostics.hpp"
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

void HomeScreen::prepareOfflineProjection() { OfflineCatalogSnapshot catalog; OfflineLibraryProjection p(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);m_fetchOfflineMovies.clear();m_fetchOfflineSnapshot=m_cachedSnapshot;for(auto &tab:m_fetchOfflineTabs){if(tab.name=="Movies")tab.rows={{"Movies",{}}};if(tab.name=="Shows")tab.rows={{"Shows",{}}};}for(auto &view:m_fetchOfflineSnapshot.shows){std::vector<MediaItem>filtered;for(const auto&i:view.items)if(p.playable(i.id)||!p.seasons(i.id).empty())filtered.push_back(i);view.items=std::move(filtered);}m_fetchOfflinePrepared=true; }
void HomeScreen::applyOfflineProjection() { const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;if(!m_fetchOfflinePrepared)return;m_tabs=std::move(m_fetchOfflineTabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_offlineSnapshot=std::move(m_fetchOfflineSnapshot);m_fetchOfflinePrepared=false;resetMediaPaging();clampNavigation(); }
void HomeScreen::applyPresentationProjection() {
    if (!m_haveCachedSnapshot) return;
    // Offline hierarchy comes from durable DownloadStore metadata.  The
    // legacy catalog is not a runtime authority; an empty catalog lets the
    // projection synthesize only the downloaded branches it needs.
    OfflineCatalogSnapshot catalog;
    OfflineLibraryProjection projection(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
    m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);
    m_fetchOfflineMovies.clear();
    m_fetchOfflineSnapshot=m_cachedSnapshot;
    for (auto &view : m_fetchOfflineSnapshot.shows) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items) {
            if (projection.playable(item.id) || !projection.seasons(item.id).empty())
                filtered.push_back(item);
        }
        view.items=std::move(filtered);
    }
    for (auto &tab : m_fetchOfflineTabs) {
        if (tab.name == "Movies") tab.rows = {{"Movies", {}}};
        if (tab.name == "Shows") tab.rows = {{"Shows", {}}};
    }
    m_fetchOfflinePrepared=true;
    applyOfflineProjection();
}
void HomeScreen::restoreOnlinePresentation() {
    if (!m_haveCachedSnapshot) return;
    const std::vector<TabData> previous=m_tabs; const int selected=m_activeTab;
    m_tabs=tabsFromSnapshot(m_cachedSnapshot);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
    resetMediaPaging();
    clampNavigation();
}

void HomeScreen::resetMediaPaging()
{
    auto reset = [](MediaPageState &state, const std::string &type, int letter) {
        if (state.cancellation)
            state.cancellation->store(true);
        state = MediaPageState{};
        state.type = type;
        state.letter = letter;
    };
    reset(m_moviePage, "movie", m_movieActiveLetter);
    reset(m_showPage, "show", m_showsActiveLetter);
    m_movieWindow.clear();
    m_showWindow.clear();
    m_animeWindow.clear();
    m_filteredShows.clear();
    m_filteredAnime.clear();
    if (const int movies = tabIndex("Movies"); movies >= 0)
        m_tabs[movies].rows = {{"Movies", {}}};
    if (const int shows = tabIndex("Shows"); shows >= 0)
        m_tabs[shows].rows = {{"Shows", {}}};
}

void HomeScreen::requestMediaPage(MediaPageState &state)
{
    if (!m_catalogDb || state.inFlight || !state.hasMore)
        return;
    if (m_catalogMetadata.scopeEpoch == 0 && m_session.valid()) {
        m_catalogMetadata.scopeEpoch = m_catalogDb->configureScope(
            m_session.serverUrl, m_session.userId);
    }
    state.cancellation = std::make_shared<std::atomic_bool>(false);
    CatalogDbJobMetadata metadata = m_catalogMetadata;
    metadata.cancellation = state.cancellation;
    state.future = m_catalogDb->readMediaPage(
        state.type, state.letter, 24, state.next, metadata);
    state.inFlight = true;
}

void HomeScreen::finishMediaPage(MediaPageState &state)
{
    if (!state.inFlight || !state.future.valid()
        || state.future.wait_for(std::chrono::milliseconds(0))
               != std::future_status::ready)
        return;
    const CatalogDbMediaPageResult result = state.future.get();
    state.inFlight = false;
    if (!result.success || result.cancelled || result.superseded)
        return;
    auto &window = state.type == "movie" ? m_moviePage.items : m_showPage.items;
    std::set<std::string> known;
    for (const auto &item : window)
        known.insert(item.id);
    for (const auto &item : result.items)
        if (known.insert(item.id).second)
            window.push_back(item);
    static constexpr std::size_t kWindowLimit = 96;
    if (window.size() > kWindowLimit) {
        const std::size_t remove = window.size() - kWindowLimit;
        window.erase(window.begin(),
                     window.begin() + static_cast<std::ptrdiff_t>(remove));
        if (state.type == "movie")
            m_activeCard = std::max(0, m_activeCard - static_cast<int>(remove));
        else {
            m_showSelected = std::max(0, m_showSelected - static_cast<int>(remove));
            m_animeSelected = std::max(0, m_animeSelected - static_cast<int>(remove));
        }
    }
    state.next = result.next;
    state.hasMore = result.hasMore;
    if (state.type == "movie") {
        m_movieWindow = window;
        refreshMovieFilter();
    } else {
        rebuildShowsPresentation();
    }
}

void HomeScreen::updateMediaPaging()
{
    finishMediaPage(m_moviePage);
    finishMediaPage(m_showPage);
    if (activeTabNamed("Movies")) {
        const auto &rows = m_tabs[tabIndex("Movies")].rows;
        const auto &items = rows.empty() ? m_movieWindow : rows[0].items;
        if (items.empty() || m_activeCard + 8 >= static_cast<int>(items.size()))
            requestMediaPage(m_moviePage);
    } else if (activeTabNamed("Shows")) {
        const int showCount = static_cast<int>(m_filteredShows.size());
        const int animeCount = static_cast<int>(m_filteredAnime.size());
        if ((m_showsFocus == ShowsFocus::AnimeGrid
             && (animeCount == 0 || m_animeSelected + 4 >= animeCount))
            || (m_showsFocus != ShowsFocus::AnimeGrid
                && (showCount == 0 || m_showSelected + 4 >= showCount)))
            requestMediaPage(m_showPage);
    }
}

static void makeMediaTabsBounded(std::vector<TabData> &tabs)
{
    for (auto &tab : tabs) {
        if (tab.name == "Movies") tab.rows = {{"Movies", {}}};
        if (tab.name == "Shows") tab.rows = {{"Shows", {}}};
    }
}

void HomeScreen::startFetch()
{
    if (m_fetchThread.joinable()) return;
    m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear(); m_fetchCacheSaved = false; m_fetchOfflinePrepared = false;
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
        std::string err; auto fail=[&](const std::string &error){m_fetchError=error;if(m_haveCachedSnapshot)prepareOfflineProjection();completeTelemetry(Outcome::Failure);m_fetchDone=true;};
        if (session.manualOfflineMode) {
            bool needsRefresh = false;
            if (!LibraryCache::load(LibraryCache::cachePath("cache", scope),
                                    m_cachedSnapshot, nullptr, &needsRefresh)) {
                fail("offline library cache unavailable");
                return;
            }
            m_haveCachedSnapshot = true;
            prepareOfflineProjection();
            m_fetchResult = offlineTabsFromSnapshot(m_cachedSnapshot);
            m_remoteSnapshot = m_cachedSnapshot;
            m_fetchCacheSaved = true;
            completeTelemetry(Outcome::Success);
            m_fetchDone = true;
            return;
        }
        bool optionalRequestFailed = false;
        std::vector<MediaItem> cw; std::string cwErr;
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12, cw, cwErr, cancellation.get());},cwErr)) { optionalRequestFailed=true; printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str()); }
        std::vector<MediaItem> ra; std::string raErr;
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLatestItems(base, token, uid, devId, 16, ra, raErr, cancellation.get());},raErr)) { optionalRequestFailed=true; printf("[HomeScreen] Recently added: %s\n", raErr.c_str()); }
        m_fetchResult=JellyfinApi::buildTabs(views,cw,ra,{},{});
        m_remoteSnapshot.continueWatching=cw;
        m_remoteSnapshot.recentlyAdded=ra;
        completeTelemetry(optionalRequestFailed ? Outcome::Failure : Outcome::Success);
        m_fetchDone=true;
    });
}
void HomeScreen::requestFetch(Uint32 now){if(m_syncSchedule.request(now))startFetch();}
void HomeScreen::finishFetch()
{
    if(m_fetchThread.joinable())m_fetchThread.join();
    m_fetchDone=false;
    if(!m_fetchError.empty()){m_libraryOffline=m_haveCachedSnapshot;if(m_libraryOffline)applyOfflineProjection();if(!m_haveCachedSnapshot)m_loadState=LoadState::Error;printf("[HomeScreen] Fetch failed: %s\n",m_fetchError.c_str());m_syncSchedule.complete(SDL_GetTicks(),false);return;}
    if(!m_fetchCacheSaved)printf("[HomeScreen] Library cache save failed; retaining old cache\n");else{m_cachedSnapshot=m_remoteSnapshot;m_haveCachedSnapshot=true;}
    const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;m_tabs=std::move(m_fetchResult);makeMediaTabsBounded(m_tabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_libraryOffline=false;if(m_session.manualOfflineMode)applyPresentationProjection();else resetMediaPaging();m_loadState=LoadState::Ready;clampNavigation();printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n",m_tabs.size(),m_fetchStats.added,m_fetchStats.changed);m_syncSchedule.complete(SDL_GetTicks(),true);
}
}
