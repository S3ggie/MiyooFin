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

bool HomeScreen::publishHierarchyCheckpoint(std::uint64_t generation)
{
    if (!m_librarySync || generation != m_hierarchyGeneration.load())
        return false;
    SyncState next=m_syncState;
    next.lastSuccessfulMs=wallClockMs();
    if (m_forceHierarchyReconcile)
        next.lastReconcileMs=next.lastSuccessfulMs;
    CatalogDbJobMetadata metadata=m_catalogMetadata;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        if (generation != m_hierarchyGeneration.load())
            return false;
        metadata.cancellation=m_catalogGenerationCancellation;
    }
    const auto result=m_librarySync->writeSyncState(
        next.lastSuccessfulMs,next.lastReconcileMs,generation,
        metadata.cancellation).get();
    if (!result.success || generation != m_hierarchyGeneration.load())
        return false;
    m_syncState=next;
    m_forceHierarchyReconcile=false;
    return true;
}

void HomeScreen::startPosterSync(const LibrarySnapshot &snapshot)
{
    queuePosterJobs(planHomePosterJobs(snapshot));
}

std::string HomeScreen::posterJobKey(const PosterJob &job)
{
    return job.itemId + ":"
        + std::to_string(static_cast<int>(job.imageType)) + ":"
        + job.imageTag + ":"
        + std::to_string(job.width) + "x"
        + std::to_string(job.height);
}

bool HomeScreen::shouldProcessPosterJob(bool highPriority, bool populationInProgress)
{
    return highPriority || !populationInProgress;
}

void HomeScreen::queuePosterJobs(std::vector<PosterJob> jobs, bool highPriority)
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    std::vector<PosterJob> newJobs;
    for (auto &job : jobs) {
        const std::string key = posterJobKey(job);
        if (m_artworkProgressKeys.insert(key).second) {
            newJobs.push_back(std::move(job));
            m_artworkTotal.fetch_add(1);
            m_artworkActive.store(true);
        }
    }
    auto &target = highPriority ? m_highPriorityPosterJobs
                                : m_lowPriorityPosterJobs;
    if (highPriority) {
        for (auto it = newJobs.rbegin(); it != newJobs.rend(); ++it)
            target.insert(target.begin(), std::move(*it));
    } else {
        for (auto &job : newJobs)
            target.push_back(std::move(job));
    }
    performanceTelemetry().setWorkerQueueDepth(
        WorkerId::HomePoster, static_cast<uint32_t>(
            m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size()));
    m_posterWake.notify_all();
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectPosterJobs(const LibrarySnapshot &snapshot)
{
    std::vector<PosterJob> out;
    for (auto &job : planHomePosterJobs(snapshot))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height))
            out.push_back(std::move(job));
    return out;
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectSeasonPosterJobs(const std::vector<MediaItem> &seasons)
{
    std::vector<PosterJob> out;
    for (auto &job : planSeasonPosterJobs(seasons))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height))
            out.push_back(std::move(job));
    return out;
}

void HomeScreen::startHierarchyCache(const LibrarySnapshot &snapshot, const LibrarySnapshot &previous,
                                     const std::set<std::string> &changedSeries)
{
    std::vector<MediaItem> all, shows; std::set<std::string> seen;
    for (const auto &view : snapshot.shows) for (const auto &show : view.items)
        if (!show.id.empty() && seen.insert(show.id).second) all.push_back(show);
    std::map<std::string,MediaItem> old;
    for(const auto&v:previous.shows)for(const auto&i:v.items)old[i.id]=i;
    for(const auto&s:all){auto it=old.find(s.id);if(m_forceHierarchyReconcile||changedSeries.count(s.id)||it==old.end()||!LibraryCache::itemEquivalent(it->second,s))shows.push_back(s);}

    std::uint64_t generation=0;
    std::shared_ptr<std::atomic_bool> cancellation;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        generation=m_hierarchyGeneration.fetch_add(1)+1;
        const std::size_t superseded=m_pendingHierarchyShows.size();
        if (m_catalogGenerationCancellation)
            m_catalogGenerationCancellation->store(true);
        cancellation=std::make_shared<std::atomic_bool>(false);
        m_catalogGenerationCancellation=cancellation;
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
    }

    bool catalogReady=false;
    if (m_librarySync) {
        const auto result=m_librarySync->reconcileSeries(all,true,cancellation).get();
        catalogReady=result.success;
    }
    m_hierarchyOffline.store(!catalogReady);

    bool noPending=false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        noPending=m_pendingHierarchyShows.empty();
    }
    if(noPending && catalogReady && !publishHierarchyCheckpoint(generation))
        m_hierarchyOffline.store(true);
    m_hierarchyWake.notify_one();
}

void HomeScreen::hierarchyWorker()
{
    for (;;) {
        std::vector<MediaItem> shows;
        std::uint64_t generation=0;
        std::shared_ptr<std::atomic_bool> catalogCancellation;
        { std::unique_lock<std::mutex> lock(m_hierarchyMutex); m_hierarchyWake.wait(lock,[&]{return m_stopHierarchyWorker||!m_pendingHierarchyShows.empty();}); if(m_stopHierarchyWorker)return; shows.swap(m_pendingHierarchyShows); generation=m_pendingHierarchyGeneration; catalogCancellation=m_catalogGenerationCancellation; performanceTelemetry().setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); performanceTelemetry().setWorkerActive(WorkerId::HomeHierarchy, true); }
        for (std::size_t showIndex=0; showIndex<shows.size(); ++showIndex) {
            const auto &series=shows[showIndex];
            { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker){ PerformanceTelemetry &telemetry=performanceTelemetry(); telemetry.addWorkerCancelled(WorkerId::HomeHierarchy, static_cast<uint32_t>(shows.size()-showIndex)); telemetry.setWorkerActive(WorkerId::HomeHierarchy, false); telemetry.setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); return; } }
            std::vector<MediaItem> cachedSeasons;
            if (m_libraryQuery) {
                const auto cached=m_libraryQuery->seasons(
                    series.id,catalogCancellation).get();
                if (cached.success) {
                    cachedSeasons=std::move(cached.items);
                    queuePosterJobs(planSeasonPosterJobs(cachedSeasons));
                    for (const auto &season : cachedSeasons)
                        (void)m_libraryQuery->episodes(
                            season.id,catalogCancellation).get();
                }
            }
            if (!m_librarySync) {
                if (generation==m_hierarchyGeneration.load()) {
                    m_hierarchyOffline.store(true);
                    performanceTelemetry().addWorkerFailed(
                        WorkerId::HomeHierarchy);
                } else {
                    performanceTelemetry().addWorkerCancelled(
                        WorkerId::HomeHierarchy);
                }
                continue;
            }
            const auto seasonRefresh=m_librarySync->refreshSeasons(
                series,catalogCancellation).get();
            if (!seasonRefresh.success) {
                const bool current=generation==m_hierarchyGeneration.load();
                if(current) m_hierarchyOffline.store(true);
                if(current && !seasonRefresh.cancelled
                   && !seasonRefresh.superseded)
                    performanceTelemetry().addWorkerFailed(
                        WorkerId::HomeHierarchy);
                else
                    performanceTelemetry().addWorkerCancelled(
                        WorkerId::HomeHierarchy);
                continue;
            }
            std::vector<MediaItem> seasons=std::move(seasonRefresh.items);
            queuePosterJobs(planSeasonPosterJobs(seasons));
            bool complete=true;
            for (const auto &season : seasons) {
                { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker){ PerformanceTelemetry &telemetry=performanceTelemetry(); telemetry.addWorkerCancelled(WorkerId::HomeHierarchy, static_cast<uint32_t>(shows.size()-showIndex)); telemetry.setWorkerActive(WorkerId::HomeHierarchy, false); telemetry.setWorkerQueueDepth(WorkerId::HomeHierarchy, 0); return; } }
                if (season.id.empty()) { complete=false; break; }
                const auto episodeRefresh=m_librarySync->refreshEpisodes(
                    series,season,catalogCancellation).get();
                if (!episodeRefresh.success) {
                    complete=false;
                    if(generation==m_hierarchyGeneration.load())
                        m_hierarchyOffline.store(true);
                    break;
                }
            }
            if (complete) {
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
                if (!publishHierarchyCheckpoint(generation))
                    m_hierarchyOffline.store(true);
            }
        }
    }
}

void HomeScreen::posterWorker()
{
    HttpClient client;
    client.setTimeoutSec(8);
    for (;;) {
        PosterJob job;
        {
            std::unique_lock<std::mutex> lock(m_posterMutex);
            m_posterWake.wait(lock,[&]{
                return m_stopPosterWorker
                    || !m_highPriorityPosterJobs.empty()
                    || (!m_initialPopulationInProgress.load()
                        && !m_lowPriorityPosterJobs.empty());
            });
            if(m_stopPosterWorker) return;
            // High-priority jobs are always processed first.
            if (!m_highPriorityPosterJobs.empty()) {
                job=std::move(m_highPriorityPosterJobs.front());
                m_highPriorityPosterJobs.erase(m_highPriorityPosterJobs.begin());
            } else if (shouldProcessPosterJob(
                       false, m_initialPopulationInProgress.load())) {
                // Only pop a low-priority job when the defer contract
                // allows it — i.e. initial population is finished.
                job=std::move(m_lowPriorityPosterJobs.front());
                m_lowPriorityPosterJobs.erase(m_lowPriorityPosterJobs.begin());
            } else {
                // Low-priority job is present but deferred; re-enter wait
                // so the CV predicate can re-check on the next notify.
                continue;
            }
            performanceTelemetry().setWorkerQueueDepth(
                WorkerId::HomePoster, static_cast<uint32_t>(
                    m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size()));
            performanceTelemetry().setWorkerActive(WorkerId::HomePoster, true);
        }

        bool complete=false;
        if(ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) {
            complete=true;
        } else {
            BinaryHttpResponse response; std::string error;
            TelemetryRequestScope request(RequestKind::Artwork); TelemetryArtworkScope artwork(ArtworkContext::HomePoster);
            if(RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,job.itemId,job.imageType,job.imageTag,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),response,error,512*1024)&&response.ok();},error)&&!response.data.empty())
                complete=ImageCache::writeToCache(job.itemId,job.imageType,job.imageTag,job.width,job.height,response.data.data(),response.data.size());
        }
        if (complete) performanceTelemetry().addWorkerCompleted(WorkerId::HomePoster);
        else performanceTelemetry().addWorkerFailed(WorkerId::HomePoster);
        m_artworkCompleted.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(m_posterMutex);
            // Erase the key on BOTH success and failure so that a later
            // queuePosterJobs call can re-admit it.  On success the
            // isCached shortcut (line 283) prevents a redundant HTTP
            // fetch when the file is still on disk; on failure the
            // network path runs again.  This also allows recovery after
            // the ImageCache janitor evicts a previously-downloaded file.
            const std::string key = posterJobKey(job);
            m_artworkProgressKeys.erase(key);
            if (m_artworkCompleted.load() >= m_artworkTotal.load()
                && m_highPriorityPosterJobs.empty()
                && m_lowPriorityPosterJobs.empty())
                m_artworkActive.store(false);
            performanceTelemetry().setWorkerQueueDepth(
                WorkerId::HomePoster, static_cast<uint32_t>(
                    m_highPriorityPosterJobs.size() + m_lowPriorityPosterJobs.size()));
        }
        performanceTelemetry().setWorkerActive(WorkerId::HomePoster, false);
    }
}

} // namespace miyoofin
