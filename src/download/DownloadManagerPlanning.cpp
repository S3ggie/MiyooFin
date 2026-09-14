#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../library/LibraryQuery.hpp"
#include "../library/LibrarySync.hpp"
#include "DownloadSupport.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"

namespace miyoofin {
namespace {
void publishDownloadGauges(const std::vector<DownloadItem> &items,
                           std::size_t plannerQueueDepth)
{
    std::uint32_t active = 0;
    std::uint32_t queued = 0;
    for (const auto &item : items) {
        if (item.state == DownloadState::Downloading)
            ++active;
        else if (item.state == DownloadState::Queued)
            ++queued;
    }
    performanceTelemetry().setDownloadGauges(
        active, queued, static_cast<std::uint32_t>(plannerQueueDepth));
}
}

DownloadPlan DownloadManager::makePlan(const std::vector<DownloadItem>&in)const{DownloadPlan p;p.filesystemFreeBytes=freeBytes();p.usableFreeBytes=p.filesystemFreeBytes>DOWNLOAD_SAFETY_RESERVE?p.filesystemFreeBytes-DOWNLOAD_SAFETY_RESERVE:0;auto s=snapshot();std::map<std::string,bool>seen;p.sizeKnown=true;for(auto i:in)if(seen.emplace(i.itemId,true).second){auto old=std::find_if(s.items.begin(),s.items.end(),[&](const DownloadItem&x){return x.itemId==i.itemId;});if(old!=s.items.end()){i.downloadedBytes=old->downloadedBytes;if(!i.hlsStorage&&old->expectedSize)i.expectedSize=old->expectedSize;}std::uint64_t planned=0;if(!plannedDownloadBytes(i,planned)){p.sizeKnown=false;p.items.push_back(i);continue;}i.expectedSize=planned;p.items.push_back(i);p.totalSourceBytes=saturatingAdd(p.totalSourceBytes,planned);p.alreadyPresentBytes=saturatingAdd(p.alreadyPresentBytes,std::min(i.downloadedBytes,planned));if(old==s.items.end())p.additionalRequiredBytes=saturatingAdd(p.additionalRequiredBytes,i.downloadedBytes>=planned?0:planned-i.downloadedBytes);}p.alreadyReservedBytes=s.reservedBytes;p.usableFreeBytes=p.usableFreeBytes>s.reservedBytes?p.usableFreeBytes-s.reservedBytes:0;p.canFit=p.sizeKnown&&p.additionalRequiredBytes<=p.usableFreeBytes;if(!p.sizeKnown)p.error="Estimated HLS size unavailable";else if(!p.canFit)p.error="Not enough space";return p;}
std::uint64_t DownloadManager::requestPlan(const std::vector<MediaItem>&items){
    std::vector<DownloadItem> provisional;
    for(const auto&m:items){DownloadItem i;i.itemId=m.id;i.itemType=m.type;i.title=m.title;i.runtimeTicks=m.runTimeTicks;i.hlsStorage=true;provisional.push_back(i);}
    DownloadPlan estimate;
    {
        UiDiagnostics::Scope scope("DownloadManager::requestPlan snapshot/filesystem estimate",false);
        estimate=makePlan(provisional);
    }
    std::unique_lock<std::mutex>l(m_mutex,std::defer_lock);
    {
        UiDiagnostics::Scope scope("DownloadManager::requestPlan mutex wait",false);
        l.lock();
    }
    std::uint64_t id=m_nextPlanId++;DownloadPlanSnapshot s;s.id=id;s.state=DownloadPlanState::Planning;s.itemCount=items.size();s.plan=estimate;m_plans[id]=s;m_planJobs.push_back({id,m_generation,m_session,items,"","",{}, {}, {}, {}, std::make_shared<std::atomic_bool>(false)});performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadPlanner,static_cast<std::uint32_t>(m_planJobs.size()));publishDownloadGauges(m_items,m_planJobs.size());m_planWake.notify_one();return id;
}
std::uint64_t DownloadManager::requestSeriesPlan(const std::string&seriesId){MediaItem series;series.id=seriesId;return requestSeriesPlan(series);}
std::uint64_t DownloadManager::requestSeasonPlan(const std::string&seriesId,const std::string&seasonId){MediaItem series,season;series.id=seriesId;season.id=seasonId;season.seriesId=seriesId;return requestSeasonPlan(series,season);}
std::uint64_t DownloadManager::requestSeriesPlan(const MediaItem &series)
{
    std::shared_ptr<library::LibraryQuery> libraryQuery;
    std::shared_ptr<library::LibrarySync> librarySync;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        libraryQuery = m_libraryQuery;
        librarySync = m_librarySync;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id=m_nextPlanId++;
    DownloadPlanSnapshot snapshot;
    snapshot.id=id;
    snapshot.state=DownloadPlanState::Planning;
    m_plans[id]=snapshot;
    m_planJobs.push_back({id,m_generation,m_session,{},series.id,"",series,{},
                          libraryQuery,librarySync,
                          std::make_shared<std::atomic_bool>(false)});
    performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadPlanner,
        static_cast<std::uint32_t>(m_planJobs.size()));
    publishDownloadGauges(m_items,m_planJobs.size());
    m_planWake.notify_one();
    return id;
}
std::uint64_t DownloadManager::requestSeasonPlan(const MediaItem &series,
                                                 const MediaItem &season)
{
    std::shared_ptr<library::LibraryQuery> libraryQuery;
    std::shared_ptr<library::LibrarySync> librarySync;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        libraryQuery = m_libraryQuery;
        librarySync = m_librarySync;
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::uint64_t id=m_nextPlanId++;
    DownloadPlanSnapshot snapshot;
    snapshot.id=id;
    snapshot.state=DownloadPlanState::Planning;
    m_plans[id]=snapshot;
    m_planJobs.push_back({id,m_generation,m_session,{},series.id,season.id,
                          series,season,libraryQuery,librarySync,
                          std::make_shared<std::atomic_bool>(false)});
    performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadPlanner,
        static_cast<std::uint32_t>(m_planJobs.size()));
    publishDownloadGauges(m_items,m_planJobs.size());
    m_planWake.notify_one();
    return id;
}
DownloadPlanSnapshot DownloadManager::planSnapshot(std::uint64_t id)const{UiDiagnostics::Scope scope("DownloadManager::planSnapshot mutex wait");std::lock_guard<std::mutex>l(m_mutex);auto it=m_plans.find(id);return it==m_plans.end()?DownloadPlanSnapshot{}:it->second;}
bool DownloadManager::tryPlanSnapshot(std::uint64_t id,DownloadPlanSnapshot&snapshot)const{std::unique_lock<std::mutex>l(m_mutex,std::try_to_lock);if(!l.owns_lock())return false;auto it=m_plans.find(id);snapshot=it==m_plans.end()?DownloadPlanSnapshot{}:it->second;return true;}

void DownloadManager::planner()
{
    for (;;) {
        PlanJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_planWake.wait(lock, [&] {
                return m_stop || !m_planJobs.empty();
            });
            if (m_stop)
                return;
            job = std::move(m_planJobs.front());
            m_planJobs.pop_front();
            m_activePlanCancellation = job.cancellation;
            performanceTelemetry().setWorkerQueueDepth(
                WorkerId::DownloadPlanner,
                static_cast<std::uint32_t>(m_planJobs.size()));
            publishDownloadGauges(m_items, m_planJobs.size());
            performanceTelemetry().setWorkerActive(WorkerId::DownloadPlanner,
                                                   true);
        }

        std::vector<MediaItem> media = job.items;
        std::vector<MediaItem> seasons;
        std::map<std::string, std::vector<MediaItem>> hierarchy;
        std::string error;
        bool hierarchyReady = false;
        bool hierarchySuperseded = false;
        const auto cancellation = job.cancellation;

        auto publishEstimate = [&](const std::vector<MediaItem> &items) {
            if (items.empty())
                return;
            std::vector<DownloadItem> provisional;
            provisional.reserve(items.size());
            for (const auto &item : items) {
                DownloadItem download;
                download.itemId = item.id;
                download.itemType = item.type;
                download.runtimeTicks = item.runTimeTicks;
                download.hlsStorage = true;
                provisional.push_back(std::move(download));
            }
            const DownloadPlan estimate = makePlan(provisional);
            std::lock_guard<std::mutex> lock(m_mutex);
            const auto plan = m_plans.find(job.id);
            if (plan != m_plans.end() && job.generation == m_generation) {
                plan->second.itemCount = items.size();
                plan->second.plan = estimate;
            }
        };

        auto queryEpisodes = [&](const std::string &seasonId,
                                 std::vector<MediaItem> &episodes) {
            if (!job.libraryQuery)
                return false;
            const library::HierarchyPage result =
                job.libraryQuery->episodes(seasonId, cancellation).get();
            if (result.superseded || result.cancelled) {
                hierarchySuperseded = true;
                return false;
            }
            if (!result.success)
                return false;
            episodes = result.items;
            return !episodes.empty();
        };

        auto refreshEpisodes = [&](const MediaItem &series,
                                   const MediaItem &season,
                                   std::vector<MediaItem> &episodes) {
            if (!job.librarySync) {
                error = "LibrarySync service unavailable";
                return false;
            }
            const library::HierarchyRefreshResult result =
                job.librarySync->refreshEpisodes(series, season,
                                                 cancellation).get();
            if (result.superseded || result.cancelled) {
                hierarchySuperseded = true;
                return false;
            }
            if (!result.success) {
                error = result.message.empty()
                    ? "LibrarySync episode refresh failed" : result.message;
                return false;
            }
            episodes = result.items;
            return true;
        };

        auto refreshSeasons = [&](const MediaItem &series,
                                  std::vector<MediaItem> &refreshed) {
            if (!job.librarySync) {
                error = "LibrarySync service unavailable";
                return false;
            }
            const library::HierarchyRefreshResult result =
                job.librarySync->refreshSeasons(series, cancellation).get();
            if (result.superseded || result.cancelled) {
                hierarchySuperseded = true;
                return false;
            }
            if (!result.success) {
                error = result.message.empty()
                    ? "LibrarySync season refresh failed" : result.message;
                return false;
            }
            refreshed = result.items;
            return true;
        };

        if (!job.seriesId.empty() && !job.seasonId.empty()) {
            std::vector<MediaItem> cached;
            hierarchyReady = queryEpisodes(job.seasonId, cached);
            if (hierarchyReady) {
                media = cached;
                MediaItem season = job.season;
                season.id = job.seasonId;
                season.seriesId = job.seriesId;
                seasons.push_back(std::move(season));
                hierarchy[job.seasonId] = media;
            }
            if (!hierarchyReady && !hierarchySuperseded) {
                media.clear();
                MediaItem season = job.season;
                season.id = job.seasonId;
                season.seriesId = job.seriesId;
                hierarchyReady = refreshEpisodes(job.series, season, media);
                if (hierarchyReady) {
                    seasons.push_back(std::move(season));
                    hierarchy[job.seasonId] = media;
                }
            }
        } else if (!job.seriesId.empty()) {
            if (job.libraryQuery) {
                const library::HierarchyPage cachedSeasons =
                    job.libraryQuery->seasons(job.seriesId, cancellation).get();
                if (cachedSeasons.superseded || cachedSeasons.cancelled) {
                    hierarchySuperseded = true;
                } else if (cachedSeasons.success
                           && !cachedSeasons.items.empty()) {
                    bool allCached = true;
                    seasons = cachedSeasons.items;
                    for (const auto &season : seasons) {
                        std::vector<MediaItem> episodes;
                        if (!queryEpisodes(season.id, episodes)) {
                            allCached = false;
                            break;
                        }
                        hierarchy[season.id] = episodes;
                        media.insert(media.end(), episodes.begin(),
                                     episodes.end());
                    }
                    hierarchyReady = allCached;
                    if (!allCached)
                        media.clear();
                }
            }
            if (!hierarchyReady && !hierarchySuperseded && error.empty()) {
                seasons.clear();
                media.clear();
                hierarchy.clear();
                hierarchyReady = refreshSeasons(job.series, seasons);
                if (hierarchyReady) {
                    for (const auto &season : seasons) {
                        std::vector<MediaItem> episodes;
                        if (!refreshEpisodes(job.series, season, episodes)) {
                            hierarchyReady = false;
                            break;
                        }
                        hierarchy[season.id] = episodes;
                        media.insert(media.end(), episodes.begin(),
                                     episodes.end());
                    }
                }
            }
        }

        if (hierarchyReady)
            publishEstimate(media);

        if (hierarchySuperseded && error.empty())
            error = "Library hierarchy plan superseded";

        std::vector<DownloadItem> out;
        std::set<std::string> seen;
        for (const auto &item : media) {
            if (!seen.insert(item.id).second)
                continue;
            std::vector<DownloadMediaSource> sources;
            if (!error.empty()
                || !RouteRequest(job.session).run(
                       [&](const std::string &base) {
                           return JellyfinApi::getDownloadMediaSources(
                               base, job.session.accessToken, job.session.userId,
                               job.session.deviceId, item.id, sources, error);
                       }, error)
                || sources.empty()) {
                if (error.empty())
                    error = "No downloadable media source";
                break;
            }
            out.push_back(makeDownloadItem(item, sources.front()));
        }

        DownloadPlan calculated;
        if (error.empty())
            calculated = makePlan(out);
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto plan = m_plans.find(job.id);
        if (plan == m_plans.end() || job.generation != m_generation) {
            if (m_activePlanCancellation == job.cancellation)
                m_activePlanCancellation.reset();
            performanceTelemetry().setWorkerActive(WorkerId::DownloadPlanner,
                                                   false);
            continue;
        }
        plan->second.itemCount = out.empty() && hierarchyReady
            ? media.size() : out.size();
        if (!error.empty()) {
            plan->second.state = DownloadPlanState::Error;
            plan->second.plan.error = error;
        } else {
            plan->second.plan = calculated;
            plan->second.state = calculated.error.empty()
                ? DownloadPlanState::Ready : DownloadPlanState::Error;
        }
        if (m_activePlanCancellation == job.cancellation)
            m_activePlanCancellation.reset();
        performanceTelemetry().setWorkerActive(WorkerId::DownloadPlanner,
                                               false);
    }
}

} // namespace miyoofin
