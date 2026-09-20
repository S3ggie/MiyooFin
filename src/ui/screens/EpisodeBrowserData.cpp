#include "EpisodeBrowserScreen.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../library/LibraryQuery.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include <algorithm>
#include <cstdio>
#include <thread>

namespace miyoofin {

void EpisodeBrowserScreen::fetchEpisodes(bool loadCachedEpisodes)
{
    if(m_fetchThread.joinable()){
        std::lock_guard<std::mutex>g(m_fetchMutex);
        if(!m_fetchDone)return;
        m_fetchThread.join();
    }
    if(m_episodes.empty())m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);
        m_fetchDone=false;
        m_cachedEpisodesDone=false;
    }
    m_fetchCancelled.store(false, std::memory_order_release);
    m_catalogCancellation->store(false, std::memory_order_release);
    const Session s=m_session;
    const MediaItem seriesItem=m_series, seasonItem=m_season;
    const std::string sid=m_series.id, season=m_season.id;
    const bool networkOffline=m_networkOffline;
    const bool downloadedOnly=m_downloadedOnly;
    const bool loadCached=loadCachedEpisodes;
    const std::shared_ptr<library::LibraryQuery> libraryQuery=m_libraryQuery;
    const std::shared_ptr<library::LibraryCoordinator> libraryCoordinator=m_libraryCoordinator;
    const std::shared_ptr<std::atomic_bool> cancellation=m_catalogCancellation;
    const std::shared_ptr<DownloadManager> downloads=m_downloads;
    m_fetchThread=std::thread([this,s,seriesItem,seasonItem,sid,season,
                                networkOffline,downloadedOnly,loadCached,
                                libraryQuery,libraryCoordinator,cancellation,downloads](){
        PerformanceTelemetry &telemetry=performanceTelemetry();
        telemetry.setWorkerActive(WorkerId::EpisodeFetch, true);
        telemetry.setWorkerQueueDepth(WorkerId::EpisodeFetch, 1);
        TelemetryTimer fetchTimer;
        bool telemetryCompleted = false;
        auto completeTelemetry = [&](Outcome outcome) noexcept {
            if (telemetryCompleted)
                return;
            telemetryCompleted = true;
            if (fetchTimer.active())
                (void)fetchTimer.elapsedUs();
            if (outcome == Outcome::Success)
                telemetry.addWorkerCompleted(WorkerId::EpisodeFetch);
            else if (outcome == Outcome::Cancelled)
                telemetry.addWorkerCancelled(WorkerId::EpisodeFetch);
            else
                telemetry.addWorkerFailed(WorkerId::EpisodeFetch);
            telemetry.setWorkerActive(WorkerId::EpisodeFetch, false);
            telemetry.setWorkerQueueDepth(WorkerId::EpisodeFetch, 0);
        };
        auto isComplete = [](DownloadState state) {
            return state == DownloadState::Complete
                || state == DownloadState::LocalOnly
                || state == DownloadState::UpdateAvailable;
        };
        auto downloadedEpisodes = [&](std::vector<MediaItem> items) {
            const DownloadSnapshot snapshot = downloads
                ? downloads->snapshot() : DownloadSnapshot{};
            std::set<std::string> completeIds;
            for (const auto &download : snapshot.items) {
                if (isComplete(download.state)
                    && download.itemType == "episode"
                    && !download.itemId.empty()
                    && download.seasonId == season
                    && (download.seriesId.empty()
                        || download.seriesId == seriesItem.id)) {
                    completeIds.insert(download.itemId);
                }
            }
            std::set<std::string> seen;
            std::vector<MediaItem> filtered;
            for (auto &item : items) {
                if (completeIds.count(item.id) && seen.insert(item.id).second)
                    filtered.push_back(std::move(item));
            }
            // A completed download may predate the hierarchy cache.  Rebuild
            // only the minimum episode metadata needed by this season view.
            for (const auto &download : snapshot.items) {
                if (!isComplete(download.state)
                    || download.itemType != "episode"
                    || download.itemId.empty()
                    || download.seasonId != season
                    || (!download.seriesId.empty()
                        && download.seriesId != seriesItem.id)
                    || !seen.insert(download.itemId).second) {
                    continue;
                }
                MediaItem item;
                item.id = download.itemId;
                item.type = "episode";
                item.title = download.title.empty()
                    ? "Episode " + std::to_string(download.episodeNumber)
                    : download.title;
                item.seriesId = download.seriesId;
                item.seriesName = download.seriesName;
                item.seasonId = download.seasonId;
                item.indexNumber = download.episodeNumber;
                item.parentIndexNumber = download.seasonNumber;
                item.playbackPositionTicks = download.playbackPositionTicks;
                item.runTimeTicks = download.runtimeTicks;
                filtered.push_back(std::move(item));
            }
            std::sort(filtered.begin(), filtered.end(),
                      [](const MediaItem &left, const MediaItem &right) {
                if (left.indexNumber != right.indexNumber)
                    return left.indexNumber < right.indexNumber;
                if (left.title != right.title)
                    return left.title < right.title;
                return left.id < right.id;
            });
            return filtered;
        };
        std::vector<MediaItem> cached;
        bool cacheReadOk = true;
        if(loadCached) {
            if (libraryQuery) {
                const library::HierarchyPage cacheResult =
                    libraryQuery->episodes(season, cancellation).get();
                cacheReadOk = cacheResult.success;
                if (cacheResult.success)
                    cached = cacheResult.items;
                else if (cacheResult.cancelled || cacheResult.superseded) {
                    completeTelemetry(Outcome::Cancelled);
                    return;
                }
            }
            if(m_fetchCancelled.load(std::memory_order_acquire)) {
                completeTelemetry(Outcome::Cancelled);
                return;
            }
            if (downloadedOnly)
                cached = downloadedEpisodes(std::move(cached));
            {
                std::lock_guard<std::mutex>g(m_fetchMutex);
                m_cachedEpisodes=cached;
                m_cachedEpisodesDone=true;
            }
            if(networkOffline) {
                {
                    std::lock_guard<std::mutex>g(m_fetchMutex);
                    m_fetchOk=cacheReadOk;
                    m_fetchEpisodes=std::move(cached);
                    m_fetchError=cacheReadOk ? "" : "CatalogDb episode read failed";
                    m_fetchDone=true;
                }
                completeTelemetry(cacheReadOk ? Outcome::Success : Outcome::Failure);
                return;
            }
        }
        if(m_fetchCancelled.load(std::memory_order_acquire)) {
            completeTelemetry(Outcome::Cancelled);
            return;
        }
        std::vector<MediaItem>v;std::string e;bool ok=false;
        if (libraryCoordinator) {
            const library::HierarchyRefreshResult refreshed =
                libraryCoordinator->refreshEpisodes(seriesItem, seasonItem,
                                                    cancellation).get();
            ok = refreshed.success;
            v = refreshed.items;
            e = refreshed.message;
        } else {
            e = "Library coordinator unavailable";
        }
        if(ok&&!m_fetchCancelled.load(std::memory_order_acquire)) {
            if (downloadedOnly)
                v = downloadedEpisodes(std::move(v));
        }
        {
            std::lock_guard<std::mutex>g(m_fetchMutex);
            m_fetchOk=ok;m_fetchEpisodes=std::move(v);m_fetchError=e;m_fetchDone=true;
        }
        completeTelemetry(m_fetchCancelled.load(std::memory_order_acquire)
            ? Outcome::Cancelled : (ok ? Outcome::Success : Outcome::Failure));
    });
}

void EpisodeBrowserScreen::publishEpisodes(std::vector<MediaItem> episodes)
{
    UiDiagnostics::Scope scope("EpisodeBrowserScreen::publishEpisodes");
    std::string selected=m_episodes.empty()?"":m_episodes[m_selectedEpisode].id;
    m_episodes=std::move(episodes);
    int index=findEpisodeIndex(m_episodes,selected);
    if(!m_initialSelectionApplied && !m_initialEpisodeId.empty()) {
        const int initial=findEpisodeIndex(m_episodes,m_initialEpisodeId);
        if(initial>=0) index=initial;
        m_initialSelectionApplied=true;
    }
    m_selectedEpisode=index>=0?index:0;
    m_loadState=LoadState::Ready;m_listScroll=0;m_overviewScroll=0;
    m_episodeArtwork={};m_episodeArtworkKey.clear();
    freePreparedArtwork(m_episodeArtworkSurface);
    std::vector<ArtworkJob> jobs;jobs.reserve(m_episodes.size());
    for(const auto&ep:m_episodes){ArtworkJob j;j.itemId=ep.id;auto t=ep.imageTags.find("Primary");if(t!=ep.imageTags.end()&&!t->second.empty()){j.imageTag=t->second;j.artworkKey=ep.id+":Primary:"+t->second+":288x162";}jobs.push_back(std::move(j));}
    {std::lock_guard<std::mutex>g(m_workerMutex);m_artworkJobs=std::move(jobs);m_failedKeys.clear();m_workerPaused=false;m_workerDecodedKey.clear();}
    clampListScroll();wakeArtworkWorker();
}

// -------------------------------------------------------------------
// clampListScroll
// -------------------------------------------------------------------
}
