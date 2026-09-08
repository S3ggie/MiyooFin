#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../net/TlsConfig.hpp"
#include "DownloadSupport.hpp"
#include "DownloadReconcile.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <cstdio>
#include <chrono>
#include <limits>
namespace miyoofin { namespace {
struct WriteCtx { FILE *f=nullptr; std::uint64_t remain=0; DownloadManager *manager=nullptr; std::string itemId,scope; std::uint64_t generation=0; std::uint64_t baseDownloaded=0; };
bool nonemptyFile(const std::string &path,std::uint64_t &size){struct stat st{};if(::stat(path.c_str(),&st)||st.st_size<=0)return false;size=(std::uint64_t)st.st_size;return true;}
size_t writeCb(char*p,size_t a,size_t b,void*u){auto*c=(WriteCtx*)u;std::uint64_t n=a*b;if(n>c->remain)return 0;size_t w=std::fwrite(p,1,(size_t)n,c->f);c->remain-=w;return w;}
int progressCb(void*u,curl_off_t total,curl_off_t,curl_off_t now,curl_off_t){auto*c=(WriteCtx*)u;if(!c->manager)return 1;auto ms=(std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();c->manager->recordProgress(c->itemId,c->scope,c->generation,c->baseDownloaded+(now>0?(std::uint64_t)now:0),ms,now>0?(std::uint64_t)now:0,total>0?(std::uint64_t)total:0);return c->manager->shouldAbort(c->itemId,c->scope,c->generation);}
}}
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
void DownloadManager::recordProgress(const std::string&id,const std::string&scope,std::uint64_t generation,std::uint64_t downloaded,std::uint64_t now,std::uint64_t currentBytes,std::uint64_t currentSize){std::lock_guard<std::mutex>l(m_mutex);if(generation!=m_generation||scope!=m_scope)return;for(auto&i:m_items)if(i.itemId==id&&i.state==DownloadState::Downloading){if(!i.hlsStorage)downloaded=std::min(downloaded,i.expectedSize);const std::uint64_t previous=i.downloadedBytes;updateRecentSpeed(m_progressSamples[id],downloaded,now,i.recentBytesPerSec);i.downloadedBytes=downloaded;if(downloaded>previous)performanceTelemetry().addDownloadBytes(downloaded-previous);if(i.hlsStorage){i.hlsCurrentSegmentBytes=currentBytes;i.hlsCurrentSegmentSize=currentSize;i.hlsActivePercent=downloadPercent(i);}return;}}
bool DownloadManager::shouldAbort(const std::string&id,const std::string&scope,std::uint64_t generation)const{std::lock_guard<std::mutex>l(m_mutex);if(m_stop||m_playback||generation!=m_generation||scope!=m_scope)return true;for(const auto&i:m_items)if(i.itemId==id)return i.state!=DownloadState::Downloading||m_deleteRequested.count(id);return true;}
bool DownloadManager::waitForHlsSegmentRetry(const std::string&id,const std::string&scope,std::uint64_t generation,unsigned seconds){
    std::unique_lock<std::mutex> l(m_mutex);
    return !m_wake.wait_for(l,std::chrono::seconds(seconds),[&]{
        if(m_stop||m_playback||generation!=m_generation||scope!=m_scope)return true;
        for(const auto&i:m_items)if(i.itemId==id)return i.state!=DownloadState::Downloading||m_deleteRequested.count(id);
        return true;
    });
}
void DownloadManager::worker(){for(;;){DownloadItem work;Session session;std::string scope;std::uint64_t generation=0,persistRevision=0;std::vector<DownloadItem> snapshot;bool persistOnly=false;{std::unique_lock<std::mutex>l(m_mutex);performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer,false);m_wake.wait(l,[&]{if(m_stop||m_persistRequested)return true;if(!m_playback)for(const auto&i:m_items)if(i.state==DownloadState::Queued)return true;return false;});if(m_stop)return;
        if(m_persistRequested){snapshot=m_items;scope=m_scope;generation=m_generation;persistRevision=m_persistRevision;persistOnly=true;}
        else {auto p=std::find_if(m_items.begin(),m_items.end(),[](const DownloadItem&i){return i.state==DownloadState::Queued;});if(p==m_items.end())continue;p->state=DownloadState::Downloading;p->recentBytesPerSec=0;m_progressSamples.erase(p->itemId);work=*p;session=m_session;scope=m_scope;generation=m_generation;snapshot=m_items;publishDownloadGauges(m_items,m_planJobs.size());performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer,true);}}
    // All snapshots are written manifest-first, then as one index.  This runs
    // only on the worker, so enqueue never performs removable-storage I/O.
    for(const auto&i:snapshot){if(i.hlsStorage)m_store.ensureHlsDirectories(scope,i.itemId);m_store.saveManifest(scope,i,nullptr);}m_store.saveIndex(scope,snapshot,nullptr);
    if(persistOnly){std::lock_guard<std::mutex>l(m_mutex);if(generation==m_generation&&scope==m_scope&&persistRevision==m_persistRevision)m_persistRequested=false;continue;}
    transfer(work,session,scope,generation);performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer,false);}}
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
        std::lock_guard<std::mutex> l(m_mutex); for(auto &i:m_items)if(i.itemId==item.itemId){i.state=failure==JellyfinApi::HlsFailure::Network?DownloadState::WaitingForNetwork:(failure==JellyfinApi::HlsFailure::Unauthorized?DownloadState::Unauthorized:DownloadState::Failed);i.recentBytesPerSec=0;m_progressSamples.erase(i.itemId);i.lastError=error;m_store.saveManifest(scope,i,nullptr);persistLocked();publishDownloadGauges(m_items,m_planJobs.size());} return false;
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
            std::string segmentUrl=urls[k]; curl_easy_setopt(c,CURLOPT_URL,segmentUrl.c_str()); curl_easy_setopt(c,CURLOPT_HTTPHEADER,sl); curl_easy_setopt(c,CURLOPT_FOLLOWLOCATION,1L); curl_easy_setopt(c,CURLOPT_MAXREDIRS,5L); curl_easy_setopt(c,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(c,CURLOPT_CONNECTTIMEOUT,15L); std::string tlsError; if(!configureTls(c,segmentUrl,&tlsError)){rc=CURLE_SSL_CACERT;std::fclose(f);curl_slist_free_all(sl);curl_easy_cleanup(c);break;} curl_easy_setopt(c,CURLOPT_LOW_SPEED_LIMIT,64L); curl_easy_setopt(c,CURLOPT_LOW_SPEED_TIME,180L); curl_easy_setopt(c,CURLOPT_WRITEFUNCTION,writeCb); curl_easy_setopt(c,CURLOPT_WRITEDATA,&ctx); curl_easy_setopt(c,CURLOPT_NOPROGRESS,0L); curl_easy_setopt(c,CURLOPT_XFERINFOFUNCTION,progressCb); curl_easy_setopt(c,CURLOPT_XFERINFODATA,&ctx);
            rc=curl_easy_perform(c); code=0; curl_easy_getinfo(c,CURLINFO_RESPONSE_CODE,&code); std::fflush(f); std::fclose(f); curl_slist_free_all(sl); curl_easy_cleanup(c);
            good=rc==CURLE_OK&&code>=200&&code<300&&nonemptyFile(part,bytes);
            const RouteRequest routes(session); const std::string lanUrl=routes.lan(), publicBase=routes.publicRoute();
            if(!good && rc!=CURLE_OK && !lanUrl.empty() && !publicBase.empty() && segmentUrl.compare(0,lanUrl.size(),lanUrl)==0){ std::printf("[Route] LAN failed; public fallback\n"); std::remove(part.c_str()); FILE *fallback=std::fopen(part.c_str(),"wb"); if(fallback){ CURL *pc=curl_easy_init(); if(pc){ curl_slist *ps=nullptr;for(const auto &h:JellyfinApi::buildAuthHeaders(session.accessToken,session.deviceId))ps=curl_slist_append(ps,h.c_str()); WriteCtx pctx{fallback,std::numeric_limits<std::uint64_t>::max(),this,item.itemId,scope,generation,item.downloadedBytes}; std::string publicUrl=RouteRequest::replaceBase(segmentUrl,lanUrl,publicBase); curl_easy_setopt(pc,CURLOPT_URL,publicUrl.c_str());curl_easy_setopt(pc,CURLOPT_HTTPHEADER,ps);curl_easy_setopt(pc,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(pc,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(pc,CURLOPT_CONNECTTIMEOUT,15L);std::string tlsError;if(!configureTls(pc,publicUrl,&tlsError)){rc=CURLE_SSL_CACERT;curl_slist_free_all(ps);curl_easy_cleanup(pc);}else{curl_easy_setopt(pc,CURLOPT_WRITEFUNCTION,writeCb);curl_easy_setopt(pc,CURLOPT_WRITEDATA,&pctx);curl_easy_setopt(pc,CURLOPT_NOPROGRESS,0L);curl_easy_setopt(pc,CURLOPT_XFERINFOFUNCTION,progressCb);curl_easy_setopt(pc,CURLOPT_XFERINFODATA,&pctx); rc=curl_easy_perform(pc);code=0;curl_easy_getinfo(pc,CURLINFO_RESPONSE_CODE,&code);curl_slist_free_all(ps);curl_easy_cleanup(pc);}} std::fflush(fallback);std::fclose(fallback); good=rc==CURLE_OK&&code>=200&&code<300&&nonemptyFile(part,bytes); } }
            if(good){std::printf("[Download] segment=%llu attempt=%u HTTP=%ld\n",(unsigned long long)k,attempt,code);break;}
            if(rc==CURLE_ABORTED_BY_CALLBACK) break;
            bool retry=hlsSegmentShouldRetry(code,(int)rc,attempt);
            std::printf("[Download] segment=%llu attempt=%u HTTP=%ld%s\n",(unsigned long long)k,attempt,code,retry?" retrying":"");
            if(!retry||!waitForHlsSegmentRetry(item.itemId,scope,generation,1u<<(attempt-1))) break;
        }
        if(!good){std::string detail="segment="+std::to_string(k)+" curl="+std::to_string((int)rc)+" "+curl_easy_strerror(rc)+" HTTP="+std::to_string(code);std::lock_guard<std::mutex>l(m_mutex);auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==item.itemId;});if(p==m_items.end())return false;if(m_deleteRequested.erase(item.itemId)){m_store.removeItem(scope,item.itemId,nullptr);m_items.erase(p);m_progressSamples.erase(item.itemId);persistLocked();publishDownloadGauges(m_items,m_planJobs.size());return false;}if(rc==CURLE_ABORTED_BY_CALLBACK){p->recentBytesPerSec=0;m_progressSamples.erase(item.itemId);m_store.reconcile(scope,*p,nullptr);persistLocked();publishDownloadGauges(m_items,m_planJobs.size());return false;}p->state=hlsSegmentFailureState(code,(int)rc);p->recentBytesPerSec=0;m_progressSamples.erase(item.itemId);p->lastError=detail;m_store.reconcile(scope,*p,nullptr);persistLocked();publishDownloadGauges(m_items,m_planJobs.size());return false;}
        if(std::rename(part.c_str(),done.c_str())) return false;
        m_store.reconcile(scope,item,nullptr); item.hlsCompletedSegments=k+1; item.hlsCurrentSegmentBytes=item.hlsCurrentSegmentSize=0; item.hlsActivePercent=downloadPercent(item); item.state=DownloadState::Downloading; {std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==item.itemId){i=item;m_store.saveManifest(scope,i,nullptr);persistLocked();}}
    }
    std::lock_guard<std::mutex>l(m_mutex); auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==item.itemId;}); if(p==m_items.end()||!m_store.validateCompletedDownload(scope,item,nullptr))return false; m_store.reconcile(scope,item,nullptr); item.state=DownloadState::Complete; item.recentBytesPerSec=0; item.lastError.clear(); *p=item; m_progressSamples.erase(item.itemId);m_store.saveManifest(scope,item,nullptr);persistLocked();publishDownloadGauges(m_items,m_planJobs.size());return true;
}}
