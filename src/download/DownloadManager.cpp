#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../cache/OfflineCatalog.hpp"
#include "../cache/LibraryCache.hpp"
#include "DownloadSupport.hpp"
#include "DownloadReconcile.hpp"
#include "../app/UiDiagnostics.hpp"
#include <curl/curl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <limits>
#include <unistd.h>
namespace miyoofin { namespace {
struct WriteCtx { FILE *f=nullptr; std::uint64_t remain=0; DownloadManager *manager=nullptr; std::string itemId,scope; std::uint64_t generation=0; std::uint64_t baseDownloaded=0; };
bool nonemptyFile(const std::string &path,std::uint64_t &size){struct stat st{};if(::stat(path.c_str(),&st)||st.st_size<=0)return false;size=(std::uint64_t)st.st_size;return true;}
size_t writeCb(char*p,size_t a,size_t b,void*u){auto*c=(WriteCtx*)u;std::uint64_t n=a*b;if(n>c->remain)return 0;size_t w=std::fwrite(p,1,(size_t)n,c->f);c->remain-=w;return w;}
int progressCb(void*u,curl_off_t total,curl_off_t,curl_off_t now,curl_off_t){auto*c=(WriteCtx*)u;if(!c->manager)return 1;auto ms=(std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();c->manager->recordProgress(c->itemId,c->scope,c->generation,c->baseDownloaded+(now>0?(std::uint64_t)now:0),ms,now>0?(std::uint64_t)now:0,total>0?(std::uint64_t)total:0);return c->manager->shouldAbort(c->itemId,c->scope,c->generation);}
}}
namespace miyoofin {
DownloadManager::DownloadManager(const Session&s,const std::string&r):m_store(r){configure(s);m_thread=std::thread(&DownloadManager::worker,this);m_planThread=std::thread(&DownloadManager::planner,this);m_reconcileThread=std::thread(&DownloadManager::reconciler,this);}
DownloadManager::~DownloadManager(){{std::lock_guard<std::mutex>l(m_mutex);m_stop=true;persistLocked();}m_wake.notify_all();m_planWake.notify_all();m_reconcileWake.notify_all();if(m_thread.joinable())m_thread.join();if(m_planThread.joinable())m_planThread.join();if(m_reconcileThread.joinable())m_reconcileThread.join();}
void DownloadManager::configure(const Session&s){std::lock_guard<std::mutex>l(m_mutex);persistLocked();++m_generation;m_session=s;m_scope=s.valid()?DownloadStore::scopeKey(s.serverUrl,s.userId):"anonymous";m_deleteRequested.clear();m_progressSamples.clear();m_persistRequested=false;
    // Never let a failed index load leak its partial result into a rebuild.
    // Both paths use independent vectors, then publish one complete result.
    std::vector<DownloadItem> loaded;
    if(!m_store.loadIndex(m_scope,loaded,nullptr)){
        loaded.clear();
        std::vector<DownloadItem> rebuilt;
        if(m_store.rebuildIndex(m_scope,rebuilt,nullptr)) loaded.swap(rebuilt);
    }
    std::set<std::string> seen;
    loaded.erase(std::remove_if(loaded.begin(),loaded.end(),[&](const DownloadItem&i){return !seen.insert(i.itemId).second;}),loaded.end());
    for(auto&i:loaded)m_store.reconcile(m_scope,i,nullptr);
    m_items.swap(loaded); persistLocked();if(s.valid())m_reconcileRequested=true;m_wake.notify_all();m_reconcileWake.notify_one();}
void DownloadManager::persistLocked(){for(const auto&i:m_items)m_store.saveManifest(m_scope,i,nullptr);m_store.saveIndex(m_scope,m_items,nullptr);}
DownloadState DownloadManager::hlsSegmentFailureState(long httpStatus,int curlCode){
    if(httpStatus==401||httpStatus==403)return DownloadState::Unauthorized;
    if(httpStatus>=400)return DownloadState::Failed;
    switch(curlCode){
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return DownloadState::WaitingForNetwork;
    default:
        return DownloadState::Failed;
    }
}
bool DownloadManager::hlsSegmentRetryable(long httpStatus,int curlCode){
    switch(httpStatus){
    case 500: case 502: case 503: case 504:
    case 520: case 521: case 522: case 523: case 524:
        return true;
    default: break;
    }
    if(httpStatus) return false;
    switch(curlCode){
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return true;
    default:
        return false;
    }
}
bool DownloadManager::hlsSegmentShouldRetry(long httpStatus,int curlCode,unsigned completedAttempts){
    return completedAttempts<HLS_SEGMENT_ATTEMPTS&&hlsSegmentRetryable(httpStatus,curlCode);
}
void DownloadManager::setPlaybackActive(bool v){{std::lock_guard<std::mutex>l(m_mutex);m_playback=v;for(auto&i:m_items)if(v&&i.state==DownloadState::Downloading){i.state=stateAfterInterrupt(DownloadInterrupt::Playback);i.recentBytesPerSec=0;m_progressSamples.erase(i.itemId);}else if(!v&&i.state==DownloadState::PausedForPlayback)i.state=DownloadState::Queued;persistLocked();}m_wake.notify_all();}
void DownloadManager::enqueue(const DownloadItem&item){enqueue(std::vector<DownloadItem>{item});}
void DownloadManager::enqueue(const std::vector<DownloadItem>&incoming){
    // This is called directly by screen input handling.  Keep it strictly
    // in-memory: manifests, index persistence, and directory creation belong
    // to worker(), before it starts the item's transfer.
    {
        std::lock_guard<std::mutex>l(m_mutex);
        for(const auto&in:incoming){
            auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==in.itemId;});
            if(p!=m_items.end())continue;
            DownloadItem i=in;
            if(i.chunkSize==0)i.chunkSize=DOWNLOAD_CHUNK_SIZE;
            i.hlsStorage=true; i.hlsProfile=HLS_PROFILE_NAME;
            estimateHlsBytes(i.runtimeTicks,i.expectedSize);
            i.recentBytesPerSec=0; i.state=DownloadState::Queued;
            m_progressSamples.erase(i.itemId);
            m_items.push_back(std::move(i));
            m_persistRequested=true;
            ++m_persistRevision;
        }
        m_reconcileRequested=true;
    }
    m_wake.notify_one();
    m_reconcileWake.notify_one();
}
void DownloadManager::pause(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&i.state!=DownloadState::Complete){i.state=stateAfterInterrupt(DownloadInterrupt::UserPause);i.recentBytesPerSec=0;m_progressSamples.erase(id);}persistLocked();m_wake.notify_all();}
void DownloadManager::resume(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&i.state!=DownloadState::Complete)i.state=DownloadState::Queued;persistLocked();m_wake.notify_one();}
void DownloadManager::retry(const std::string&id){resume(id);}
void DownloadManager::requestReconcile(){{std::lock_guard<std::mutex>l(m_mutex);if(m_session.valid())m_reconcileRequested=true;}m_reconcileWake.notify_one();}
bool DownloadManager::redownload(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&i.state==DownloadState::UpdateAvailable&&!i.availableMediaSourceId.empty()&&(i.hlsStorage||i.availableSize)){if(!m_store.removePartialBytes(m_scope,id,nullptr))return false;i.mediaSourceId=i.availableMediaSourceId;i.sourceEtag=i.availableSourceEtag;if(i.hlsStorage)estimateHlsBytes(i.runtimeTicks,i.expectedSize);else i.expectedSize=i.availableSize;i.availableMediaSourceId.clear();i.availableSourceEtag.clear();i.availableSize=0;i.downloadedBytes=0;i.recentBytesPerSec=0;m_progressSamples.erase(id);i.state=DownloadState::Queued;i.updateAvailable=false;i.localOnly=false;m_store.saveManifest(m_scope,i,nullptr);persistLocked();m_wake.notify_one();return true;}return false;}
bool DownloadManager::erase(const std::string&id,std::string*e){std::lock_guard<std::mutex>l(m_mutex);auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==id;});if(p==m_items.end())return false;if(p->state==DownloadState::Downloading){m_deleteRequested.insert(id);p->state=DownloadState::Paused;p->recentBytesPerSec=0;m_progressSamples.erase(id);persistLocked();m_wake.notify_all();return true;}bool ok=m_store.removeItem(m_scope,id,e);if(ok){m_items.erase(p);m_progressSamples.erase(id);}persistLocked();return ok;}
std::uint64_t DownloadManager::freeBytes()const{struct statvfs s{};std::string path=m_store.scopePath(m_scope);while(!path.empty()){if(!statvfs(path.c_str(),&s))return (std::uint64_t)s.f_bavail*(std::uint64_t)s.f_frsize;size_t slash=path.find_last_of('/');if(slash==std::string::npos)break;path.resize(slash);}return 0;}
DownloadSnapshot DownloadManager::snapshot()const{std::lock_guard<std::mutex>l(m_mutex);DownloadSnapshot x;x.items=m_items;x.freeBytes=freeBytes();x.playbackActive=m_playback;for(auto&i:x.items){if(i.state!=DownloadState::Complete&&i.state!=DownloadState::LocalOnly&&i.state!=DownloadState::UpdateAvailable){x.reservedBytes=saturatingAdd(x.reservedBytes,queueRemainingBytes(i));}else x.localBytes=saturatingAdd(x.localBytes,i.hlsStorage?i.downloadedBytes:(i.expectedSize?i.expectedSize:i.downloadedBytes));}return x;}
bool DownloadManager::updateRecentSpeed(RecentSpeedSample &sample,std::uint64_t downloaded,std::uint64_t now,std::uint64_t &bytesPerSec){
    static constexpr std::uint64_t WINDOW_MS=1500;
    if(sample.samples.empty()||downloaded<sample.downloadedBytes||now<sample.samples.back().first){sample={};sample.downloadedBytes=downloaded;sample.lastReceivedMs=now;sample.samples.push_back({now,downloaded});return false;}
    if(downloaded==sample.downloadedBytes){
        if(now>=sample.lastReceivedMs+WINDOW_MS && bytesPerSec){bytesPerSec=0;return true;}
        return false;
    }
    sample.downloadedBytes=downloaded; sample.lastReceivedMs=now; sample.samples.push_back({now,downloaded});
    while(sample.samples.size()>1 && now-sample.samples[1].first>WINDOW_MS) sample.samples.pop_front();
    const auto &first=sample.samples.front();
    if(now<=first.first) return false;
    const std::uint64_t rate=(downloaded-first.second)*1000/(now-first.first);
    if(rate==bytesPerSec) return false;
    bytesPerSec=rate;
    return true;
}
void DownloadManager::recordProgress(const std::string&id,const std::string&scope,std::uint64_t generation,std::uint64_t downloaded,std::uint64_t now,std::uint64_t currentBytes,std::uint64_t currentSize){std::lock_guard<std::mutex>l(m_mutex);if(generation!=m_generation||scope!=m_scope)return;for(auto&i:m_items)if(i.itemId==id&&i.state==DownloadState::Downloading){if(!i.hlsStorage)downloaded=std::min(downloaded,i.expectedSize);updateRecentSpeed(m_progressSamples[id],downloaded,now,i.recentBytesPerSec);i.downloadedBytes=downloaded;if(i.hlsStorage){i.hlsCurrentSegmentBytes=currentBytes;i.hlsCurrentSegmentSize=currentSize;i.hlsActivePercent=downloadPercent(i);}return;}}
bool DownloadManager::hasComplete(const std::string&id)const{std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&(i.state==DownloadState::Complete||i.state==DownloadState::LocalOnly||i.state==DownloadState::UpdateAvailable)&&m_store.validateCompletedDownload(m_scope,i,nullptr))return true;return false;}
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
    std::uint64_t id=m_nextPlanId++;DownloadPlanSnapshot s;s.id=id;s.state=DownloadPlanState::Planning;s.itemCount=items.size();s.plan=estimate;m_plans[id]=s;m_planJobs.push_back({id,m_generation,m_session,items,"","",{}, {}});m_planWake.notify_one();return id;
}
std::uint64_t DownloadManager::requestSeriesPlan(const std::string&seriesId){MediaItem series;series.id=seriesId;return requestSeriesPlan(series);}
std::uint64_t DownloadManager::requestSeasonPlan(const std::string&seriesId,const std::string&seasonId){MediaItem series,season;series.id=seriesId;season.id=seasonId;season.seriesId=seriesId;return requestSeasonPlan(series,season);}
std::uint64_t DownloadManager::requestSeriesPlan(const MediaItem&series){OfflineCatalogSnapshot cache;std::vector<MediaItem> cached;if(OfflineCatalog::load(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),cache)){auto it=cache.seasonsBySeries.find(series.id);if(it!=cache.seasonsBySeries.end())for(const auto&season:it->second){auto eps=cache.episodesBySeason.find(season.id);if(eps==cache.episodesBySeason.end()){cached.clear();break;}cached.insert(cached.end(),eps->second.begin(),eps->second.end());}}std::vector<DownloadItem> provisional;for(const auto&m:cached){DownloadItem i;i.itemId=m.id;i.runtimeTicks=m.runTimeTicks;i.hlsStorage=true;provisional.push_back(i);}DownloadPlan estimate=cached.empty()?DownloadPlan{}:makePlan(provisional);std::lock_guard<std::mutex>l(m_mutex);std::uint64_t id=m_nextPlanId++;DownloadPlanSnapshot s;s.id=id;s.state=DownloadPlanState::Planning;s.itemCount=cached.size();s.plan=estimate;m_plans[id]=s;m_planJobs.push_back({id,m_generation,m_session,{},series.id,"",series,{}});m_planWake.notify_one();return id;}
std::uint64_t DownloadManager::requestSeasonPlan(const MediaItem&series,const MediaItem&season){OfflineCatalogSnapshot cache;std::vector<MediaItem> cached;auto path=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));if(OfflineCatalog::load(path,cache)){auto it=cache.episodesBySeason.find(season.id);if(it!=cache.episodesBySeason.end())cached=it->second;}std::vector<DownloadItem> provisional;for(const auto&m:cached){DownloadItem i;i.itemId=m.id;i.runtimeTicks=m.runTimeTicks;i.hlsStorage=true;provisional.push_back(i);}DownloadPlan estimate=cached.empty()?DownloadPlan{}:makePlan(provisional);std::lock_guard<std::mutex>l(m_mutex);std::uint64_t id=m_nextPlanId++;DownloadPlanSnapshot s;s.id=id;s.state=DownloadPlanState::Planning;s.itemCount=cached.size();s.plan=estimate;m_plans[id]=s;m_planJobs.push_back({id,m_generation,m_session,{},series.id,season.id,series,season});m_planWake.notify_one();return id;}
DownloadPlanSnapshot DownloadManager::planSnapshot(std::uint64_t id)const{UiDiagnostics::Scope scope("DownloadManager::planSnapshot mutex wait");std::lock_guard<std::mutex>l(m_mutex);auto it=m_plans.find(id);return it==m_plans.end()?DownloadPlanSnapshot{}:it->second;}
bool DownloadManager::tryPlanSnapshot(std::uint64_t id,DownloadPlanSnapshot&snapshot)const{std::unique_lock<std::mutex>l(m_mutex,std::try_to_lock);if(!l.owns_lock())return false;auto it=m_plans.find(id);snapshot=it==m_plans.end()?DownloadPlanSnapshot{}:it->second;return true;}
void DownloadManager::reconciler(){for(;;){Session session;std::string scope;std::uint64_t generation=0;std::vector<DownloadItem> items;{std::unique_lock<std::mutex>l(m_mutex);m_reconcileWake.wait(l,[&]{return m_stop||m_reconcileRequested;});if(m_stop)return;m_reconcileWake.wait_for(l,std::chrono::seconds(3),[&]{return m_stop;});if(m_stop)return;m_reconcileRequested=false;session=m_session;scope=m_scope;generation=m_generation;items=m_items;}for(const auto&old:items){if(!session.valid())break;std::vector<DownloadMediaSource> sources;std::string error;SourceCheck result=SourceCheck::Transient;if(RouteRequest(session).run([&](const std::string&base){return JellyfinApi::getDownloadMediaSources(base,session.accessToken,session.userId,session.deviceId,old.itemId,sources,error);},error)&&!sources.empty())result=SourceCheck::Same;else if(error.find("HTTP 404")!=std::string::npos)result=SourceCheck::Missing;else if(error=="Unauthorized")result=SourceCheck::Unauthorized;const DownloadMediaSource*source=result==SourceCheck::Same?&sources.front():nullptr;std::lock_guard<std::mutex>l(m_mutex);if(m_stop||generation!=m_generation||scope!=m_scope)break;auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==old.itemId;});if(p==m_items.end()||p->state==DownloadState::Downloading)continue;bool discard=reconcileSource(*p,result,source);if(discard)m_store.removePartialBytes(scope,p->itemId,nullptr);m_store.saveManifest(scope,*p,nullptr);persistLocked();}}}
void DownloadManager::planner(){for(;;){PlanJob job;{std::unique_lock<std::mutex>l(m_mutex);m_planWake.wait(l,[&]{return m_stop||!m_planJobs.empty();});if(m_stop)return;job=std::move(m_planJobs.front());m_planJobs.pop_front();}std::vector<MediaItem> media=job.items,seasons;std::map<std::string,std::vector<MediaItem> > hierarchy;std::string error;if(!job.seasonId.empty()){if(RouteRequest(job.session).run([&](const std::string&base){return JellyfinApi::getEpisodes(base,job.session.accessToken,job.session.userId,job.session.deviceId,job.seriesId,job.seasonId,media,error);},error)){MediaItem season=job.season;season.id=job.seasonId;season.seriesId=job.seriesId;seasons.push_back(season);hierarchy[job.seasonId]=media;}}else if(!job.seriesId.empty()){if(RouteRequest(job.session).run([&](const std::string&base){return JellyfinApi::getSeasons(base,job.session.accessToken,job.session.userId,job.session.deviceId,job.seriesId,seasons,error);},error))for(const auto&season:seasons){std::vector<MediaItem> episodes;if(!RouteRequest(job.session).run([&](const std::string&base){return JellyfinApi::getEpisodes(base,job.session.accessToken,job.session.userId,job.session.deviceId,job.seriesId,season.id,episodes,error);},error))break;hierarchy[season.id]=episodes;media.insert(media.end(),episodes.begin(),episodes.end());}}if(error.empty()&&!job.seriesId.empty()){MediaItem series=job.series;series.id=job.seriesId;if(series.title.empty())for(const auto&a:hierarchy)if(!a.second.empty()){series.title=a.second.front().seriesName;break;}OfflineCatalog::storeDiscoveredHierarchy(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(job.session.serverUrl,job.session.userId)),series,seasons,hierarchy,true,nullptr);}std::vector<DownloadItem> out;std::set<std::string> seen;for(const auto&m:media){if(!seen.insert(m.id).second)continue;std::vector<DownloadMediaSource> sources;if(!RouteRequest(job.session).run([&](const std::string&base){return JellyfinApi::getDownloadMediaSources(base,job.session.accessToken,job.session.userId,job.session.deviceId,m.id,sources,error);},error)||sources.empty()){if(error.empty())error="No downloadable media source";break;}out.push_back(makeDownloadItem(m,sources.front()));}DownloadPlan calculated; if(error.empty()) calculated=makePlan(out);std::lock_guard<std::mutex>l(m_mutex);auto it=m_plans.find(job.id);if(it==m_plans.end()||job.generation!=m_generation)continue;it->second.itemCount=out.size();if(!error.empty()){it->second.state=DownloadPlanState::Error;it->second.plan.error=error;}else{it->second.plan=calculated;it->second.state=calculated.error.empty()?DownloadPlanState::Ready:DownloadPlanState::Error;}}}
bool DownloadManager::shouldAbort(const std::string&id,const std::string&scope,std::uint64_t generation)const{std::lock_guard<std::mutex>l(m_mutex);if(m_stop||m_playback||generation!=m_generation||scope!=m_scope)return true;for(const auto&i:m_items)if(i.itemId==id)return i.state!=DownloadState::Downloading||m_deleteRequested.count(id);return true;}
bool DownloadManager::waitForHlsSegmentRetry(const std::string&id,const std::string&scope,std::uint64_t generation,unsigned seconds){
    std::unique_lock<std::mutex> l(m_mutex);
    return !m_wake.wait_for(l,std::chrono::seconds(seconds),[&]{
        if(m_stop||m_playback||generation!=m_generation||scope!=m_scope)return true;
        for(const auto&i:m_items)if(i.itemId==id)return i.state!=DownloadState::Downloading||m_deleteRequested.count(id);
        return true;
    });
}
void DownloadManager::worker(){for(;;){DownloadItem work;Session session;std::string scope;std::uint64_t generation=0,persistRevision=0;std::vector<DownloadItem> snapshot;bool persistOnly=false;{std::unique_lock<std::mutex>l(m_mutex);m_wake.wait(l,[&]{if(m_stop||m_persistRequested)return true;if(!m_playback)for(const auto&i:m_items)if(i.state==DownloadState::Queued)return true;return false;});if(m_stop)return;
        if(m_persistRequested){snapshot=m_items;scope=m_scope;generation=m_generation;persistRevision=m_persistRevision;persistOnly=true;}
        else {auto p=std::find_if(m_items.begin(),m_items.end(),[](const DownloadItem&i){return i.state==DownloadState::Queued;});if(p==m_items.end())continue;p->state=DownloadState::Downloading;p->recentBytesPerSec=0;m_progressSamples.erase(p->itemId);work=*p;session=m_session;scope=m_scope;generation=m_generation;snapshot=m_items;}}
    // All snapshots are written manifest-first, then as one index.  This runs
    // only on the worker, so enqueue never performs removable-storage I/O.
    for(const auto&i:snapshot){if(i.hlsStorage)m_store.ensureHlsDirectories(scope,i.itemId);m_store.saveManifest(scope,i,nullptr);}m_store.saveIndex(scope,snapshot,nullptr);
    if(persistOnly){std::lock_guard<std::mutex>l(m_mutex);if(generation==m_generation&&scope==m_scope&&persistRevision==m_persistRevision)m_persistRequested=false;continue;}
    transfer(work,session,scope,generation);}}
bool DownloadManager::transfer(DownloadItem&item,const Session&session,const std::string&scope,std::uint64_t generation){
    if(!session.valid()) return false;
    // A v1 payload is only retained when it is already complete.  New work is
    // always HLS and must never issue the original /Download request.
    if(!item.hlsStorage){
        if(item.state==DownloadState::Complete&&m_store.validateCompletedDownload(scope,item,nullptr)) return true;
        m_store.removePartialBytes(scope,item.itemId,nullptr);
        item.hlsStorage=true; item.hlsSegmentCount=0; item.downloadedBytes=0; estimateHlsBytes(item.runtimeTicks,item.expectedSize);
        item.hlsProfile=HLS_PROFILE_NAME;
    }
    std::vector<std::string> urls; std::string error; JellyfinApi::HlsFailure failure;
    if(!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getHlsSegmentUrls(base,session.accessToken,session.deviceId,item.itemId,item.mediaSourceId,urls,error,&failure);},error)){
        std::lock_guard<std::mutex> l(m_mutex); for(auto &i:m_items)if(i.itemId==item.itemId){i.state=failure==JellyfinApi::HlsFailure::Network?DownloadState::WaitingForNetwork:(failure==JellyfinApi::HlsFailure::Unauthorized?DownloadState::Unauthorized:DownloadState::Failed);i.recentBytesPerSec=0;m_progressSamples.erase(i.itemId);i.lastError=error;m_store.saveManifest(scope,i,nullptr);persistLocked();} return false;
    }
    item.hlsSegmentCount=urls.size(); m_store.ensureHlsDirectories(scope,item.itemId);
    // Persist discovery before fetching segment zero: first-segment transcodes
    // may take a while, but the manifest must retain the discovered playlist.
    {std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==item.itemId){i.hlsSegmentCount=item.hlsSegmentCount;i.hlsStorage=true;m_store.saveManifest(scope,i,nullptr);persistLocked();break;}}
    m_store.reconcile(scope,item,nullptr);
    item.hlsCompletedSegments=m_store.firstIncompleteSegment(scope,item);
    item.hlsCurrentSegmentBytes=item.hlsCurrentSegmentSize=0;
    item.hlsActivePercent=downloadPercent(item);
    item.state=DownloadState::Downloading;
    {std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==item.itemId){i=item;m_store.saveManifest(scope,i,nullptr);persistLocked();break;}}
    for(std::uint64_t k=m_store.firstIncompleteSegment(scope,item);k<urls.size();++k){
        if(shouldAbort(item.itemId,scope,generation)) return false;
        std::string part=m_store.segmentPath(scope,item.itemId,k,true), done=m_store.segmentPath(scope,item.itemId,k);
        CURLcode rc=CURLE_OK; long code=0; std::uint64_t bytes=0; bool good=false;
        for(unsigned attempt=1;attempt<=HLS_SEGMENT_ATTEMPTS;++attempt){
            // "wb" deliberately replaces a failed response body before every retry.
            FILE *f=std::fopen(part.c_str(),"wb"); if(!f){rc=CURLE_WRITE_ERROR;break;}
            CURL *c=curl_easy_init(); if(!c){std::fclose(f);rc=CURLE_FAILED_INIT;break;}
            auto headers=JellyfinApi::buildAuthHeaders(session.accessToken,session.deviceId); curl_slist *sl=nullptr; for(auto &h:headers)sl=curl_slist_append(sl,h.c_str());
            WriteCtx ctx{f,std::numeric_limits<std::uint64_t>::max(),this,item.itemId,scope,generation,item.downloadedBytes};
            std::string segmentUrl=urls[k]; curl_easy_setopt(c,CURLOPT_URL,segmentUrl.c_str()); curl_easy_setopt(c,CURLOPT_HTTPHEADER,sl); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L); curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L); curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L); curl_easy_setopt(c,CURLOPT_SSL_VERIFYPEER,0L); curl_easy_setopt(c,CURLOPT_SSL_VERIFYHOST,0L); curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,64L); curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,180L); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,writeCb); curl_easy_setopt(c,CURLOPT_WRITEDATA,&ctx); curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L); curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progressCb); curl_easy_setopt(c,CURLOPT_XFERINFODATA,&ctx);
            rc=curl_easy_perform(c); code=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code); std::fflush(f); std::fclose(f); curl_slist_free_all(sl); curl_easy_cleanup(c);
            good=rc==CURLE_OK&&code>=200&&code<300&&nonemptyFile(part,bytes);
            if(!good && rc!=CURLE_OK && !session.localServerUrl.empty() && segmentUrl.compare(0,session.localServerUrl.size(),session.localServerUrl)==0){ std::printf("[Route] LAN failed; public fallback\n"); std::remove(part.c_str()); FILE *fallback=std::fopen(part.c_str(),"wb"); if(fallback){ CURL *pc=curl_easy_init(); if(pc){ curl_slist *ps=nullptr;for(const auto &h:JellyfinApi::buildAuthHeaders(session.accessToken,session.deviceId))ps=curl_slist_append(ps,h.c_str()); WriteCtx pctx{fallback,std::numeric_limits<std::uint64_t>::max(),this,item.itemId,scope,generation,item.downloadedBytes}; std::string publicUrl=RouteRequest::replaceBase(segmentUrl,session.localServerUrl,session.serverUrl); curl_easy_setopt(pc,CURLOPT_URL,publicUrl.c_str());curl_easy_setopt(pc,CURLOPT_HTTPHEADER,ps);curl_easy_setopt(pc,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(pc,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(pc,CURLOPT_CONNECTTIMEOUT,15L);curl_easy_setopt(pc,CURLOPT_SSL_VERIFYPEER,0L);curl_easy_setopt(pc,CURLOPT_SSL_VERIFYHOST,0L);curl_easy_setopt(pc,CURLOPT_WRITEFUNCTION,writeCb);curl_easy_setopt(pc,CURLOPT_WRITEDATA,&pctx);curl_easy_setopt(pc,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(pc,CURLOPT_XFERINFOFUNCTION,progressCb);curl_easy_setopt(pc,CURLOPT_XFERINFODATA,&pctx); rc=curl_easy_perform(pc);code=0;curl_easy_getinfo(pc,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(ps);curl_easy_cleanup(pc);} std::fflush(fallback);std::fclose(fallback); good=rc==CURLE_OK&&code>=200&&code<300&&nonemptyFile(part,bytes); } }
            if(good){std::printf("[Download] segment=%llu attempt=%u HTTP=%ld\n",(unsigned long long)k,attempt,code);break;}
            if(rc==CURLE_ABORTED_BY_CALLBACK) break;
            bool retry=hlsSegmentShouldRetry(code,(int)rc,attempt);
            std::printf("[Download] segment=%llu attempt=%u HTTP=%ld%s\n",(unsigned long long)k,attempt,code,retry?" retrying":"");
            if(!retry||!waitForHlsSegmentRetry(item.itemId,scope,generation,1u<<(attempt-1))) break;
        }
        if(!good){std::string detail="segment="+std::to_string(k)+" curl="+std::to_string((int)rc)+" "+curl_easy_strerror(rc)+" HTTP="+std::to_string(code);std::lock_guard<std::mutex>l(m_mutex);auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==item.itemId;});if(p==m_items.end())return false;if(m_deleteRequested.erase(item.itemId)){m_store.removeItem(scope,item.itemId,nullptr);m_items.erase(p);m_progressSamples.erase(item.itemId);persistLocked();return false;}if(rc==CURLE_ABORTED_BY_CALLBACK){p->recentBytesPerSec=0;m_progressSamples.erase(item.itemId);m_store.reconcile(scope,*p,nullptr);persistLocked();return false;}p->state=hlsSegmentFailureState(code,(int)rc);p->recentBytesPerSec=0;m_progressSamples.erase(item.itemId);p->lastError=detail;m_store.reconcile(scope,*p,nullptr);persistLocked();return false;}
        if(std::rename(part.c_str(),done.c_str())) return false;
        m_store.reconcile(scope,item,nullptr); item.hlsCompletedSegments=k+1; item.hlsCurrentSegmentBytes=item.hlsCurrentSegmentSize=0; item.hlsActivePercent=downloadPercent(item); item.state=DownloadState::Downloading; {std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==item.itemId){i=item;m_store.saveManifest(scope,i,nullptr);persistLocked();}}
    }
    std::lock_guard<std::mutex>l(m_mutex); auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==item.itemId;}); if(p==m_items.end()||!m_store.validateCompletedDownload(scope,item,nullptr))return false; m_store.reconcile(scope,item,nullptr); item.state=DownloadState::Complete; item.recentBytesPerSec=0; item.lastError.clear(); *p=item; m_progressSamples.erase(item.itemId);m_store.saveManifest(scope,item,nullptr);persistLocked();return true;
}}
