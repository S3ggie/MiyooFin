#include "DownloadManager.hpp"
#include "DownloadSupport.hpp"
#include <sys/statvfs.h>
#include <algorithm>
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
bool DownloadManager::hasComplete(const std::string&id)const{std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&(i.state==DownloadState::Complete||i.state==DownloadState::LocalOnly||i.state==DownloadState::UpdateAvailable)&&m_store.validateCompletedDownload(m_scope,i,nullptr))return true;return false;}
}
