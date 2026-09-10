#include "HomeScreen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}

std::vector<MediaItem> HomeScreen::cachedSeasonsForSeries(const std::string &seriesId) const
{
    std::vector<MediaItem> seasons;
    OfflineCatalogSnapshot catalog;
    {
        std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
        if (!m_catalogSnapshotReady) return {};
        const auto seasonsIt=m_catalogSnapshot.seasonsBySeries.find(seriesId);
        if (seasonsIt==m_catalogSnapshot.seasonsBySeries.end()) return {};
        seasons=seasonsIt->second;
        if (presentationOffline()) {
            catalog.seasonsBySeries.emplace(seriesId,seasons);
            for (const auto &season:seasons) {
                const auto episodesIt=m_catalogSnapshot.episodesBySeason.find(season.id);
                if (episodesIt!=m_catalogSnapshot.episodesBySeason.end())
                    catalog.episodesBySeason.emplace(season.id,episodesIt->second);
            }
        }
    }
    if (presentationOffline()) {
        LibrarySnapshot library;
        OfflineLibraryProjection projection(library,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
        return projection.seasons(seriesId);
    }
    return seasons;
}

void HomeScreen::startPosterSync(const LibrarySnapshot &snapshot)
{
    queuePosterJobs(collectPosterJobs(snapshot));
}

void HomeScreen::queuePosterJobs(std::vector<PosterJob> jobs)
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    std::set<std::string> queued;
    for (const auto &job : m_pendingPosterJobs)
        queued.insert(job.itemId + ":" + job.imageTag + ":" + std::to_string(job.width) + "x" + std::to_string(job.height));
    for (auto &job : jobs) {
        std::string key=job.itemId + ":" + job.imageTag + ":" + std::to_string(job.width) + "x" + std::to_string(job.height);
        if (queued.insert(key).second && !ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height))
            m_pendingPosterJobs.push_back(std::move(job));
    }
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomePoster, static_cast<uint32_t>(m_pendingPosterJobs.size()));
    m_posterWake.notify_one();
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectPosterJobs(const LibrarySnapshot &snapshot)
{
    std::vector<PosterJob> out;
    for (auto &job : planHomePosterJobs(snapshot))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) out.push_back(std::move(job));
    return out;
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectSeasonPosterJobs(const std::vector<MediaItem> &seasons)
{
    std::vector<PosterJob> out;
    for (auto &job : planSeasonPosterJobs(seasons))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) out.push_back(std::move(job));
    return out;
}

void HomeScreen::startHierarchyCache(const LibrarySnapshot &snapshot, const LibrarySnapshot &previous,
                                     const std::set<std::string> &changedSeries)
{
    std::vector<MediaItem> all, shows; std::set<std::string> seen;
    for (const auto &view : snapshot.shows) for (const auto &show : view.items)
        if (!show.id.empty() && seen.insert(show.id).second) all.push_back(show);
    OfflineCatalogSnapshot catalogSnapshot;
    const std::string catalog=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    const bool catalogValid=OfflineCatalog::load(catalog,catalogSnapshot);
    std::map<std::string,MediaItem> old;
    for(const auto&v:previous.shows)for(const auto&i:v.items)old[i.id]=i;
    for(const auto&s:all){auto it=old.find(s.id);if(!catalogValid||m_forceHierarchyReconcile||changedSeries.count(s.id)||it==old.end()||!LibraryCache::itemEquivalent(it->second,s))shows.push_back(s);}
    // An authoritative top-level list also provides deletion reconciliation;
    // this remains background work and never affects download files.
    OfflineCatalog::reconcileSeries(catalog,all,nullptr);
    // This function runs from the library fetch worker.  Refresh the reusable
    // RAM catalog after reconciliation, never from Home's SDL-thread push.
    if (OfflineCatalog::load(catalog,catalogSnapshot,nullptr)) {
        std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
        m_catalogSnapshot=std::move(catalogSnapshot);
        m_catalogSnapshotReady=true;
    }
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    const std::uint64_t generation=m_hierarchyGeneration.fetch_add(1)+1;
    const std::size_t superseded=m_pendingHierarchyShows.size();
    if (m_catalogGenerationCancellation)
        m_catalogGenerationCancellation->store(true);
    m_catalogGenerationCancellation =
        std::make_shared<std::atomic_bool>(false);
    m_pendingHierarchyShows=std::move(shows); // a newer library snapshot supersedes queued work
    m_pendingHierarchyGeneration=generation;
    m_hierarchyCompleted.store(0);
    m_hierarchyTotal.store(m_pendingHierarchyShows.size());
    m_hierarchyActive.store(!m_pendingHierarchyShows.empty());
    PerformanceTelemetry &telemetry=performanceTelemetry();
    telemetry.setWorkerQueueDepth(WorkerId::HomeHierarchy,
                                  static_cast<uint32_t>(m_pendingHierarchyShows.size()));
    telemetry.setWorkerActive(WorkerId::HomeHierarchy, !m_pendingHierarchyShows.empty());
    if (superseded != 0)
        telemetry.addWorkerCancelled(WorkerId::HomeHierarchy,
                                     static_cast<uint32_t>(superseded));
    if(m_pendingHierarchyShows.empty()) {
        m_syncState.lastSuccessfulMs=wallClockMs();
        if(m_forceHierarchyReconcile)m_syncState.lastReconcileMs=m_syncState.lastSuccessfulMs;
        SyncStateStore::save(SyncStateStore::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_syncState);
    }
    m_hierarchyOffline.store(false);
    m_hierarchyWake.notify_one();
}

void HomeScreen::hierarchyWorker()
{
    for (;;) {
        std::vector<MediaItem> shows;
        std::uint64_t generation=0;
        std::shared_ptr<std::atomic_bool> catalogCancellation;
        { std::unique_lock<std::mutex> lock(m_hierarchyMutex); m_hierarchyWake.wait(lock,[&]{return m_stopHierarchyWorker||!m_pendingHierarchyShows.empty();}); if(m_stopHierarchyWorker)return; shows.swap(m_pendingHierarchyShows); generation=m_pendingHierarchyGeneration; catalogCancellation=m_catalogGenerationCancellation; performanceTelemetry().setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); performanceTelemetry().setWorkerActive(WorkerId::HomeHierarchy, true); }
        const std::string catalog=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
        for (std::size_t showIndex=0; showIndex<shows.size(); ++showIndex) {
            const auto &series=shows[showIndex];
            { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker){ PerformanceTelemetry &telemetry=performanceTelemetry(); telemetry.addWorkerCancelled(WorkerId::HomeHierarchy, static_cast<uint32_t>(shows.size()-showIndex)); telemetry.setWorkerActive(WorkerId::HomeHierarchy, false); telemetry.setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); return; } }
            std::vector<MediaItem> seasons; std::string error;
            if (!RouteRequest(m_session).run([&](const std::string &base){return JellyfinApi::getSeasons(base,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,seasons,error);},error)) { const bool current=generation==m_hierarchyGeneration.load(); if(current) m_hierarchyOffline.store(true); if(current) performanceTelemetry().addWorkerFailed(WorkerId::HomeHierarchy); else performanceTelemetry().addWorkerCancelled(WorkerId::HomeHierarchy); continue; }
            queuePosterJobs(collectSeasonPosterJobs(seasons));
            std::map<std::string,std::vector<MediaItem> > episodesBySeason;
            bool complete=true;
            for (const auto &season : seasons) {
                { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker){ PerformanceTelemetry &telemetry=performanceTelemetry(); telemetry.addWorkerCancelled(WorkerId::HomeHierarchy, static_cast<uint32_t>(shows.size()-showIndex)); telemetry.setWorkerActive(WorkerId::HomeHierarchy, false); telemetry.setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); return; } }
                if (season.id.empty()) { complete=false; break; }
                std::vector<MediaItem> episodes; error.clear();
                if (!RouteRequest(m_session).run([&](const std::string &base){return JellyfinApi::getEpisodes(base,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,season.id,episodes,error);},error)) { complete=false; if(generation==m_hierarchyGeneration.load()) m_hierarchyOffline.store(true); break; }
                episodesBySeason[season.id]=std::move(episodes);
            }
            // A show is only complete after every discovered level has been
            // fetched and atomically merged into the offline catalog.
            if (complete && OfflineCatalog::storeDiscoveredHierarchy(catalog,series,seasons,episodesBySeason,true,nullptr)) {
                // The same complete subtree is submitted to CatalogDb on this
                // worker. The CatalogDb worker owns validation and the single
                // series transaction; the UI thread never waits for it.
                auto catalogWrite = submitCatalogHierarchy(
                    series, seasons, episodesBySeason, generation, true,
                    catalogCancellation);
                const auto catalogResult = catalogWrite.get();
                if (!catalogResult.success) {
                    if (generation == m_hierarchyGeneration.load()) {
                        if (catalogResult.cancelled || catalogResult.superseded)
                            performanceTelemetry().addWorkerCancelled(
                                WorkerId::HomeHierarchy);
                        else
                            performanceTelemetry().addWorkerFailed(
                                WorkerId::HomeHierarchy);
                    } else {
                        performanceTelemetry().addWorkerCancelled(
                            WorkerId::HomeHierarchy);
                    }
                    continue;
                }
                // Reload on this background worker so the RAM snapshot exactly
                // follows catalog merge semantics and is ready for handoff.
                OfflineCatalogSnapshot snapshot;
                if (OfflineCatalog::load(catalog,snapshot,nullptr)) {
                    std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
                    m_catalogSnapshot=std::move(snapshot);
                    m_catalogSnapshotReady=true;
                }
                if (generation==m_hierarchyGeneration.load()) {
                    m_hierarchyCompleted.fetch_add(1);
                    performanceTelemetry().addWorkerCompleted(WorkerId::HomeHierarchy);
                } else {
                    performanceTelemetry().addWorkerCancelled(WorkerId::HomeHierarchy);
                }
            } else {
                if (generation==m_hierarchyGeneration.load())
                    performanceTelemetry().addWorkerFailed(WorkerId::HomeHierarchy);
                else
                    performanceTelemetry().addWorkerCancelled(WorkerId::HomeHierarchy);
            }
        }
        if (generation==m_hierarchyGeneration.load()) {
            m_hierarchyActive.store(false);
            performanceTelemetry().setWorkerActive(WorkerId::HomeHierarchy, false);
            performanceTelemetry().setWorkerQueueDepth(WorkerId::HomeHierarchy, 0);
            // A watermark means the requested hierarchy was fully committed,
            // never merely that the metadata request happened.  Failures keep
            // the old checkpoint so the next online attempt is conservative.
            if (!m_hierarchyOffline.load() && m_hierarchyCompleted.load()==m_hierarchyTotal.load()) {
                m_syncState.lastSuccessfulMs=wallClockMs();
                if(m_forceHierarchyReconcile)m_syncState.lastReconcileMs=m_syncState.lastSuccessfulMs;
                SyncStateStore::save(SyncStateStore::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_syncState);
                m_forceHierarchyReconcile=false;
            }
        }
    }
}

void HomeScreen::posterWorker()
{
    for (;;) {
        std::vector<PosterJob> jobs;
        { std::unique_lock<std::mutex> lock(m_posterMutex); m_posterWake.wait(lock,[&]{return m_stopPosterWorker||!m_pendingPosterJobs.empty();}); if(m_stopPosterWorker) return; jobs.swap(m_pendingPosterJobs); performanceTelemetry().setWorkerQueueDepth(WorkerId::HomePoster, 0); performanceTelemetry().setWorkerActive(WorkerId::HomePoster, true); }
        for(const auto &job:jobs){ HttpClient client;client.setTimeoutSec(8);BinaryHttpResponse response;std::string error; TelemetryRequestScope request(RequestKind::Artwork); TelemetryArtworkScope artwork(ArtworkContext::HomePoster); if(RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,job.itemId,job.imageType,job.imageTag,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),response,error,512*1024)&&response.ok();},error)&&!response.data.empty()){ if(ImageCache::writeToCache(job.itemId,job.imageType,job.imageTag,job.width,job.height,response.data.data(),response.data.size())) performanceTelemetry().addWorkerCompleted(WorkerId::HomePoster); else performanceTelemetry().addWorkerFailed(WorkerId::HomePoster); } else performanceTelemetry().addWorkerFailed(WorkerId::HomePoster); }
        performanceTelemetry().setWorkerActive(WorkerId::HomePoster, false);
        std::lock_guard<std::mutex> lock(m_posterMutex);
        performanceTelemetry().setWorkerQueueDepth(
            WorkerId::HomePoster, static_cast<uint32_t>(m_pendingPosterJobs.size()));
    }
}

} // namespace miyoofin
