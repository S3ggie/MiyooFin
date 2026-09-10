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
#include <cstdio>
#include <ctime>

namespace miyoofin {

static constexpr std::int64_t HIERARCHY_RECONCILE_MS=24LL*60*60*1000;
static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}

void HomeScreen::prepareOfflineProjection() { OfflineCatalogSnapshot catalog; OfflineLibraryProjection p(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);m_fetchOfflineMovies=p.movies();m_fetchOfflineSnapshot=m_cachedSnapshot;for(auto &view:m_fetchOfflineSnapshot.shows){std::vector<MediaItem>filtered;for(const auto&i:view.items)if(p.playable(i.id)||!p.seasons(i.id).empty())filtered.push_back(i);view.items=std::move(filtered);}m_fetchOfflinePrepared=true; }
void HomeScreen::applyOfflineProjection() { const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;if(!m_fetchOfflinePrepared)return;m_tabs=std::move(m_fetchOfflineTabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_movieMaster=std::move(m_fetchOfflineMovies);m_offlineSnapshot=std::move(m_fetchOfflineSnapshot);m_fetchOfflinePrepared=false;refreshMovieFilter();rebuildShowsPresentation();clampNavigation(); }
void HomeScreen::applyPresentationProjection() {
    if (!m_haveCachedSnapshot) return;
    // Offline hierarchy comes from durable DownloadStore metadata.  The
    // legacy catalog is not a runtime authority; an empty catalog lets the
    // projection synthesize only the downloaded branches it needs.
    OfflineCatalogSnapshot catalog;
    OfflineLibraryProjection projection(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
    m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);
    m_fetchOfflineMovies=projection.movies();
    m_fetchOfflineSnapshot=m_cachedSnapshot;
    for (auto &view : m_fetchOfflineSnapshot.shows) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items) {
            if (projection.playable(item.id) || !projection.seasons(item.id).empty())
                filtered.push_back(item);
        }
        view.items=std::move(filtered);
    }
    m_fetchOfflinePrepared=true;
    applyOfflineProjection();
}
void HomeScreen::restoreOnlinePresentation() {
    if (!m_haveCachedSnapshot) return;
    const std::vector<TabData> previous=m_tabs; const int selected=m_activeTab;
    m_tabs=tabsFromSnapshot(m_cachedSnapshot);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
    m_movieMaster=combineMovieViews(m_cachedSnapshot.movies);
    refreshMovieFilter(); rebuildShowsPresentation(); clampNavigation();
}

void HomeScreen::startFetch()
{
    if (m_fetchThread.joinable()) return;
    m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear(); m_fetchCacheSaved = false; m_fetchOfflinePrepared = false;
    Session session=m_session; std::string url=session.serverUrl; std::string token=m_session.accessToken; std::string uid=m_session.userId; std::string devId=m_session.deviceId;
    m_fetchThread = std::thread([this, session, url, token, uid, devId]() {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::HomeLibraryFetch, true);
        telemetry.setWorkerQueueDepth(WorkerId::HomeLibraryFetch, 1);
        const std::string scope=LibraryCache::scopeKey(url,uid);
        if (m_catalogDb) {
            CatalogDbJobMetadata metadata=m_catalogMetadata;
            auto cached=m_catalogDb->readLibrarySnapshot(metadata).get();
            if (cached.success) { m_cachedSnapshot=std::move(cached.snapshot); m_haveCachedSnapshot=true; }
        }
        SyncState legacyState;
        const bool legacyAvailable=SyncStateStore::load(
            SyncStateStore::path("cache",scope),legacyState,nullptr);
        CatalogDbSyncState catalogState;
        if (m_catalogDb) {
            CatalogDbJobMetadata metadata=m_catalogMetadata;
            catalogState=m_catalogDb->readSyncState(
                legacyAvailable,legacyState.lastSuccessfulMs,
                legacyState.lastReconcileMs,metadata).get();
            if (catalogState.success) {
                m_syncState.lastSuccessfulMs=catalogState.lastSuccessfulMs;
                m_syncState.lastReconcileMs=catalogState.lastReconcileMs;
            }
        } else if (legacyAvailable) {
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
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getViews(base, token, uid, devId, views, err);},err)){fail(err);return;}
        printf("[HomeScreen] Got %zu library views\n", views.size());
        std::vector<MediaItem> cw; std::string cwErr;
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12, cw, cwErr);},cwErr)) printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());
        std::vector<MediaItem> ra; std::string raErr;
        ++requestCount;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLatestItems(base, token, uid, devId, 16, ra, raErr);},raErr)) printf("[HomeScreen] Recently added: %s\n", raErr.c_str());
        std::vector<std::pair<std::string,std::vector<MediaItem>>> moviesByView, showsByView; LibrarySnapshot snapshot; snapshot.continueWatching=cw; snapshot.recentlyAdded=ra;
        for (const auto &v:views) {
            if (v.collectionType=="movies") { std::vector<MediaItem> items; std::string ie; ++requestCount; if(RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLibraryItems(base,token,uid,devId,v.id,"Movie",50,items,ie);},ie)){mediaCount += static_cast<uint32_t>(items.size());moviesByView.push_back({v.name,std::move(items)});snapshot.movies.push_back({v.id,v.name,v.collectionType,moviesByView.back().second});} else {fail(ie);return;} }
            else if (v.collectionType=="tvshows") { std::vector<MediaItem> items; std::string ie; ++requestCount; if(RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLibraryItems(base,token,uid,devId,v.id,"Series",50,items,ie);},ie)){mediaCount += static_cast<uint32_t>(items.size());showsByView.push_back({v.name,std::move(items)});snapshot.shows.push_back({v.id,v.name,v.collectionType,showsByView.back().second});} else {fail(ie);return;} }
        }
        m_fetchResult=JellyfinApi::buildTabs(views,cw,ra,moviesByView,showsByView); m_remoteSnapshot=std::move(snapshot); std::set<std::string> changedSeries;
        if(m_syncState.lastSuccessfulMs>0&&!m_forceHierarchyReconcile){std::vector<MediaItem> changed;std::string changedError;++requestCount;if(!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getChangedHierarchyItems(base,token,uid,devId,m_syncState.lastSuccessfulMs,changed,changedError);},changedError)){fail(changedError);return;}changedHierarchyCount = static_cast<uint32_t>(changed.size());for(const auto&i:changed){if(i.type=="show")changedSeries.insert(i.id);else if(!i.seriesId.empty())changedSeries.insert(i.seriesId);}}
        std::vector<StalePoster> stale; m_fetchStats=LibraryCache::reconcile(m_cachedSnapshot,m_remoteSnapshot,&stale);
        if(m_catalogDb && m_catalogDb->seedLibrarySnapshot(m_remoteSnapshot,m_catalogMetadata).get().success){m_fetchCacheSaved=true;cacheSaved=true;for(const auto&p:stale)ImageCache::removeCached(p.itemId,ImageType::Primary,p.tag,64,96);startPosterSync(m_remoteSnapshot);startHierarchyCache(m_remoteSnapshot,m_cachedSnapshot,changedSeries);}
        completeTelemetry(Outcome::Success);
        m_fetchDone=true;
    });
}
void HomeScreen::requestFetch(Uint32 now){if(m_syncSchedule.request(now))startFetch();}
void HomeScreen::finishFetch()
{
    if(m_fetchThread.joinable())m_fetchThread.join(); m_fetchDone=false;
    if(!m_fetchError.empty()){m_libraryOffline=m_haveCachedSnapshot;if(m_libraryOffline)applyOfflineProjection();if(!m_haveCachedSnapshot)m_loadState=LoadState::Error;printf("[HomeScreen] Fetch failed: %s\n",m_fetchError.c_str());m_syncSchedule.complete(SDL_GetTicks(),false);return;}
    if(!m_fetchCacheSaved)printf("[HomeScreen] Library cache save failed; retaining old cache\n");else{m_cachedSnapshot=m_remoteSnapshot;m_haveCachedSnapshot=true;}
    const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;m_tabs=std::move(m_fetchResult);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_libraryOffline=false;m_movieMaster=combineMovieViews(m_cachedSnapshot.movies);refreshMovieFilter();rebuildShowsPresentation();if(m_session.manualOfflineMode)applyPresentationProjection();m_loadState=LoadState::Ready;clampNavigation();printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n",m_tabs.size(),m_fetchStats.added,m_fetchStats.changed);m_syncSchedule.complete(SDL_GetTicks(),true);
}
}
