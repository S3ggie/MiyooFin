#include "HomeScreen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"

namespace miyoofin {

bool HomeScreen::requestHierarchy(const std::vector<MediaItem> &shows,
                                  std::uint64_t generation,
                                  bool forceReconcile)
{
    if (!m_libraryCoordinator)
        return false;

    std::lock_guard<std::mutex> lock(m_hierarchyStateMutex);
    if (m_hierarchySubmissionClosed || generation == 0
        || m_hierarchyActive.load()
        || (m_fetchCancellation && m_fetchCancellation->load()))
        return false;
    std::vector<MediaItem> pending;
    for (const auto &show : shows) {
        if (!show.id.empty() && show.type == "show"
            && !m_seasonPrefetchedIds.count(show.id))
            pending.push_back(show);
    }

    if (pending.empty())
        return false;

    std::uint64_t request = 0;
    if (!m_libraryCoordinator->requestHierarchy(
            pending, generation, forceReconcile, request))
        return false;

    m_hierarchyRequest.store(request);
    m_hierarchyCompleted.store(0);
    m_hierarchyTotal.store(pending.size());
    m_hierarchyOffline.store(false);
    m_hierarchyActive.store(true);
    m_hierarchyRequestReady.store(true);
    return true;
}

void HomeScreen::consumeHierarchyResults()
{
    if (!m_libraryCoordinator || !m_hierarchyRequestReady.load())
        return;

    const std::uint64_t request = m_hierarchyRequest.load();
    library::HierarchyResult result;
    while (m_libraryCoordinator->takeHierarchyResult(request, result)) {
        if (result.request != request)
            continue;

        // A top-level catalog commit supersedes every hierarchy walk started
        // against the previous catalog epoch.  Do not queue stale artwork or
        // record a stale successful series as prefetched.  A stale terminal
        // still closes the local request so the next fetch can submit work.
        if (result.generation != committedCatalogGeneration()) {
            if (result.terminal) {
                m_hierarchyActive.store(false);
                m_hierarchyRequestReady.store(false);
            }
            continue;
        }

        // Cached seasons are published before the network refresh so the
        // artwork queue remains cache-first even when the server is slow or
        // temporarily unavailable.
        if (!result.cachedSeasons.empty())
            queuePosterJobs(collectSeasonPosterJobs(result.cachedSeasons));
        if (!result.seasons.empty())
            queuePosterJobs(collectSeasonPosterJobs(result.seasons));

        if (!result.terminal) {
            if (result.cacheOnly)
                continue;
            if (result.success) {
                {
                    std::lock_guard<std::mutex> lock(m_hierarchyStateMutex);
                    m_seasonPrefetchedIds.insert(result.seriesId);
                }
                m_hierarchyCompleted.fetch_add(1);
            } else if (!result.cancelled && !result.superseded) {
                m_hierarchyOffline.store(true);
            }
            continue;
        }

        if (result.checkpointCommitted) {
            m_syncState.lastSuccessfulMs = result.lastSuccessfulMs;
            m_syncState.lastReconcileMs = result.lastReconcileMs;
        } else if (!result.success && !result.cancelled
                   && !result.superseded) {
            m_hierarchyOffline.store(true);
        }
        m_hierarchyActive.store(false);
        m_hierarchyRequestReady.store(false);
    }
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
            if(RouteRequest(m_session).run([&](const std::string &base){
                    return client.getBinary(buildImageUrl(base,job.itemId,job.imageType,job.imageTag,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),
                    response,error,512*1024)&&response.ok();},error)&&!response.data.empty())
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
