#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "DownloadReconcile.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"

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

void DownloadManager::requestReconcile(){{std::lock_guard<std::mutex>l(m_mutex);if(m_session.valid()){m_reconcileRequested=true;performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile,1);publishDownloadGauges(m_items,m_planJobs.size());}}m_reconcileWake.notify_one();}

void DownloadManager::reconciler(){for(;;){Session session;std::string scope;std::uint64_t generation=0;bool startupReconcile=false;std::vector<DownloadItem> items;{std::unique_lock<std::mutex>l(m_mutex);m_reconcileWake.wait(l,[&]{return m_stop||m_reconcileRequested;});if(m_stop)return;m_reconcileWake.wait_for(l,std::chrono::seconds(3),[&]{return m_stop;});if(m_stop)return;m_reconcileRequested=false;startupReconcile=m_startupReconcile;m_startupReconcile=false;performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile,0);performanceTelemetry().setWorkerActive(WorkerId::DownloadReconcile,true);session=m_session;scope=m_scope;generation=m_generation;items=m_items;}const std::uint64_t nowMs=TelemetryClock::monotonicUs()/1000;std::vector<std::pair<std::string,bool>> changed;bool aborted=false;for(const auto&old:items){if(!session.valid())break;if(reconcileShouldSkip(old,startupReconcile,nowMs))continue;std::vector<DownloadMediaSource> sources;std::string error;SourceCheck result=SourceCheck::Transient;if(RouteRequest(session).run([&](const std::string&base){return JellyfinApi::getDownloadMediaSources(base,session.accessToken,session.userId,session.deviceId,old.itemId,sources,error);},error)&&!sources.empty())result=SourceCheck::Same;else if(error.find("HTTP 404")!=std::string::npos)result=SourceCheck::Missing;else if(error=="Unauthorized")result=SourceCheck::Unauthorized;const DownloadMediaSource*source=result==SourceCheck::Same?&sources.front():nullptr;{std::lock_guard<std::mutex>l(m_mutex);if(m_stop||generation!=m_generation||scope!=m_scope){aborted=true;break;}auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==old.itemId;});if(p==m_items.end()||p->state==DownloadState::Downloading)continue;const bool discard=applyReconciledSource(*p,result,source,nowMs);if(discard)m_store.removePartialBytes(scope,p->itemId,nullptr);changed.push_back({old.itemId,discard});}}
// One persist pass per reconcile: manifests only for items this pass touched,
// then a single index write.  Stale partial-byte removal happens atomically
// with the in-memory apply above (same critical section) so a wake in between
// cannot start the new source's transfer over mixed old/new segment files;
// only the manifest/index writes are deferred here.
{std::lock_guard<std::mutex>l(m_mutex);if(!aborted&&!m_stop&&generation==m_generation&&scope==m_scope&&!changed.empty()){for(const auto&c:changed){auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==c.first;});if(p==m_items.end()||p->state==DownloadState::Downloading)continue;m_store.saveManifest(scope,*p,nullptr);}m_store.saveIndex(scope,m_items,nullptr);publishDownloadGauges(m_items,m_planJobs.size());}}
performanceTelemetry().setWorkerActive(WorkerId::DownloadReconcile,false);}}

} // namespace miyoofin
