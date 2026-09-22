#include "HomeScreen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../cache/ImageCache.hpp"

namespace miyoofin {

bool HomeScreen::requestHierarchy(const std::vector<MediaItem>& shows, std::uint64_t generation,
                                  bool forceReconcile)
{
    if (!m_libraryCoordinator)
        return false;

    std::lock_guard<std::mutex> lock(m_hierarchyStateMutex);
    if (m_hierarchySubmissionClosed || generation == 0 || m_hierarchyActive.load() ||
        (m_libraryFetch && m_libraryFetch->cancelled()))
        return false;
    std::vector<MediaItem> pending;
    for (const auto& show : shows) {
        if (!show.id.empty() && show.type == "show" && !m_seasonPrefetchedIds.count(show.id))
            pending.push_back(show);
    }

    if (pending.empty())
        return false;

    std::uint64_t request = 0;
    if (!m_libraryCoordinator->requestHierarchy(pending, generation, forceReconcile, request))
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
            if (m_artworkController)
                m_artworkController->queuePosterJobs(collectSeasonPosterJobs(result.cachedSeasons));
        if (!result.seasons.empty())
            if (m_artworkController)
                m_artworkController->queuePosterJobs(collectSeasonPosterJobs(result.seasons));

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

        if (!result.checkpointCommitted && !result.success && !result.cancelled &&
            !result.superseded) {
            m_hierarchyOffline.store(true);
        }
        m_hierarchyActive.store(false);
        m_hierarchyRequestReady.store(false);
    }
}

std::string HomeScreen::posterJobKey(const PosterJob& job)
{
    return job.itemId + ":" + std::to_string(static_cast<int>(job.imageType)) + ":" + job.imageTag +
           ":" + std::to_string(job.width) + "x" + std::to_string(job.height);
}

bool HomeScreen::shouldProcessPosterJob(bool highPriority, bool populationInProgress)
{
    return HomeArtworkController::shouldProcessPosterJob(highPriority, populationInProgress);
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectPosterJobs(const LibrarySnapshot& snapshot)
{
    std::vector<PosterJob> out;
    for (auto& job : planHomePosterJobs(snapshot)) {
        if (!ImageCache::isCached(job.itemId, job.imageType, job.imageTag, job.width, job.height))
            out.push_back(std::move(job));
    }
    return out;
}

std::vector<HomeScreen::PosterJob>
HomeScreen::collectSeasonPosterJobs(const std::vector<MediaItem>& seasons)
{
    std::vector<PosterJob> out;
    for (auto& job : planSeasonPosterJobs(seasons)) {
        if (!ImageCache::isCached(job.itemId, job.imageType, job.imageTag, job.width, job.height))
            out.push_back(std::move(job));
    }
    return out;
}

} // namespace miyoofin
