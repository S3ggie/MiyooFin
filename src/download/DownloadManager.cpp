#include "DownloadManager.hpp"
#include "DownloadSupport.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include <sys/statvfs.h>
#include <algorithm>
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
DownloadManager::DownloadManager(const Session&s,const std::string&r):m_store(r){configure(s);m_thread=std::thread(&DownloadManager::worker,this);
        m_planThread=std::thread(&DownloadManager::planner,this);m_reconcileThread=std::thread(&DownloadManager::reconciler,this);}
DownloadManager::~DownloadManager(){{std::lock_guard<std::mutex>l(m_mutex);m_stop=true;if(m_activePlanCancellation)m_activePlanCancellation->store(true);
            for(auto &job:m_planJobs)if(job.cancellation)job.cancellation->store(true);
            // Whole-library write justified: rare lifecycle event (shutdown);
            // every steady-state transition already persisted per-item/index
            // above, so this only converges any last in-memory state.
            persistLocked();}m_wake.notify_all();m_planWake.notify_all();m_reconcileWake.notify_all();
        if(m_thread.joinable())m_thread.join();
        if(m_planThread.joinable())m_planThread.join();
        if(m_reconcileThread.joinable())m_reconcileThread.join();}
// configure() switches scope and rebuilds the id set from disk, so both
// persistLocked() calls below stay whole-library (rare lifecycle event;
// see persistLocked()).  Steady-state transitions never take this path.
void DownloadManager::configure(const Session&s){std::lock_guard<std::mutex>l(m_mutex);if(m_activePlanCancellation)m_activePlanCancellation->store(true);
        for(auto &job:m_planJobs)if(job.cancellation)job.cancellation->store(true);
        persistLocked();++m_generation;m_session=s;
        m_scope=s.valid()?DownloadStore::scopeKey(s.serverUrl,s.userId):"anonymous";m_deleteRequested.clear();m_progressSamples.clear();m_persistRequested=false;m_persistPendingIds.clear();
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
    m_items.swap(loaded);
    // Seed the durable-manifest set from the successful load/rebuild: every
    // id here has a readable manifest on disk.  persistLocked() below only
    // ever adds (a failed re-save leaves the old manifest in place), so the
    // set stays consistent even if a rewrite fails.
    m_indexedIds.clear();for(const auto&i:m_items)m_indexedIds.insert(i.itemId);
    persistLocked();if(s.valid()){m_reconcileRequested=true;m_startupReconcile=true;}publishDownloadGauges(m_items,m_planJobs.size());
        if(s.valid())performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile,1);
        m_wake.notify_all();m_reconcileWake.notify_one();}
void DownloadManager::saveIndexLocked(){std::vector<DownloadItem> indexed;indexed.reserve(m_items.size());
        for(const auto&i:m_items)if(m_indexedIds.count(i.itemId))indexed.push_back(i);m_store.saveIndex(m_scope,indexed,nullptr);}
void DownloadManager::persistLocked(){for(const auto&i:m_items)if(m_store.saveManifest(m_scope,i,nullptr))m_indexedIds.insert(i.itemId);saveIndexLocked();}
// persistLocked() rewrites the whole library (every manifest + the index,
// each fsync).  It stays ONLY for rare lifecycle events that rebuild the id
// set or shut down: configure() (scope/load) and the destructor.  Every
// steady-state transition below uses persistItemLocked() (one small atomic
// manifest write, milliseconds under the mutex) or a single saveIndex() when
// the id set itself changed, so SDL-thread callers never block on slow
// whole-library SD writes and no stale whole-library pass can revert a
// newer per-item state.
// Segment completions only touch one item's bytes, and the index lists item
// ids (unchanged by segment progress), so the per-segment path persists just
// that item's manifest instead of rewriting the whole library.  The manifest
// stays crash-safe via atomic write+fsync; segment files on disk remain the
// source of truth that startup reconcile rebuilds from.
void DownloadManager::persistItemLocked(const std::string&id){for(const auto&i:m_items)if(i.itemId==id){if(m_store.saveManifest(m_scope,i,nullptr))m_indexedIds.insert(id);break;}}
void DownloadManager::setPlaybackActive(bool v){{std::lock_guard<std::mutex>l(m_mutex);m_playback=v;
            // Multi-item transition, but still per-item: each affected item's
            // manifest is written individually under the lock (never the whole
            // library), so an unrelated item's on-disk state is untouched.
            for(auto&i:m_items)if(v&&i.state==DownloadState::Downloading){i.state=stateAfterInterrupt(DownloadInterrupt::Playback);i.recentBytesPerSec=0;m_progressSamples.erase(i.itemId);
                if(m_store.saveManifest(m_scope,i,nullptr))m_indexedIds.insert(i.itemId);
                }else if(!v&&i.state==DownloadState::PausedForPlayback){i.state=DownloadState::Queued;if(m_store.saveManifest(m_scope,i,nullptr))m_indexedIds.insert(i.itemId);}
            publishDownloadGauges(m_items,m_planJobs.size());}m_wake.notify_all();}
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
            m_persistPendingIds.insert(m_items.back().itemId);
        }
        m_reconcileRequested=true;
        performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile,1);
        publishDownloadGauges(m_items,m_planJobs.size());
    }
    m_wake.notify_one();
    m_reconcileWake.notify_one();
}
void DownloadManager::pause(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);
        for(auto&i:m_items)if(i.itemId==id&&i.state!=DownloadState::Complete){i.state=stateAfterInterrupt(DownloadInterrupt::UserPause);i.recentBytesPerSec=0;m_progressSamples.erase(id);
            }persistItemLocked(id);
            // Crash durability (uniform with the worker pass via
            // m_indexedIds/saveIndexLocked): the pausing transition durably
            // wrote this manifest above, so its index write travels with it.
            // Unconditional on purpose: between the pass's pending-swap and
            // its final index write the id is no longer "pending", yet the
            // pass's index is not durable — skipping here would still orphan
            // the manifest on a crash in that window.
            saveIndexLocked();
            publishDownloadGauges(m_items,m_planJobs.size());m_wake.notify_all();}
void DownloadManager::resume(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);for(auto&i:m_items)if(i.itemId==id&&i.state!=DownloadState::Complete)i.state=DownloadState::Queued;
        persistItemLocked(id);
        // Same crash-durability argument as pause() above: a durably written
        // manifest always carries its index write, so no pending-set check
        // can leave a manifest orphaned by a crash before the worker's pass
        // index lands.
        saveIndexLocked();
        publishDownloadGauges(m_items,m_planJobs.size());m_wake.notify_one();}
void DownloadManager::retry(const std::string&id){resume(id);}
bool DownloadManager::redownload(const std::string&id){std::lock_guard<std::mutex>l(m_mutex);
        for(auto&i:m_items)if(i.itemId==id&&i.state==DownloadState::UpdateAvailable&&!i.availableMediaSourceId.empty()&&(i.hlsStorage||i.availableSize)){
            if(!m_store.removePartialBytes(m_scope,id,nullptr))return false;
            i.mediaSourceId=i.availableMediaSourceId;i.sourceEtag=i.availableSourceEtag;
            if(i.hlsStorage)estimateHlsBytes(i.runtimeTicks,i.expectedSize);else i.expectedSize=i.availableSize;i.availableMediaSourceId.clear();i.availableSourceEtag.clear();
            i.availableSize=0;i.downloadedBytes=0;i.recentBytesPerSec=0;m_progressSamples.erase(id);i.state=DownloadState::Queued;i.updateAvailable=false;i.localOnly=false;
            persistItemLocked(i.itemId);publishDownloadGauges(m_items,m_planJobs.size());m_wake.notify_one();return true;}return false;}
bool DownloadManager::erase(const std::string&id,std::string*e){std::lock_guard<std::mutex>l(m_mutex);
        auto p=std::find_if(m_items.begin(),m_items.end(),[&](const DownloadItem&i){return i.itemId==id;});if(p==m_items.end())return false;
        if(p->state==DownloadState::Downloading){m_deleteRequested.insert(id);p->state=DownloadState::Paused;p->recentBytesPerSec=0;m_progressSamples.erase(id);persistItemLocked(id);
            publishDownloadGauges(m_items,m_planJobs.size());m_wake.notify_all();return true;}m_deleteRequested.erase(id);bool ok=m_store.removeItem(m_scope,id,e);if(ok){m_items.erase(p);
            m_progressSamples.erase(id);m_indexedIds.erase(id);m_persistPendingIds.erase(id);}// Id set changed: only the index is rewritten (the removed
            // manifest is already gone via removeItem); unrelated manifests
            // are untouched.  The index is rebuilt from the durable-manifest
            // set, so it can never retain the removed id.
            saveIndexLocked();publishDownloadGauges(m_items,m_planJobs.size());return ok;}
bool DownloadManager::statvfsFreeBytes(const DownloadStore&store,const std::string&scope,std::uint64_t&out){struct statvfs s{};std::string path=store.scopePath(scope);
        while(!path.empty()){if(!statvfs(path.c_str(),&s)){out=(std::uint64_t)s.f_bavail*(std::uint64_t)s.f_frsize;return true;}size_t slash=path.find_last_of('/');
            if(slash==std::string::npos)break;
            path.resize(slash);}return false;}
std::uint64_t DownloadManager::freeBytes()const{
    const std::uint64_t nowMs=TelemetryClock::monotonicUs()/1000;
    std::string scope; std::uint64_t cached=0, cachedAt=0;
    {std::lock_guard<std::mutex>l(m_mutex);scope=m_scope;cached=m_freeBytesCached;cachedAt=m_freeBytesCachedAtMs;
     if(!freeSpaceCacheExpired(nowMs,cachedAt))return cached;}
    // statvfs (with its parent-dir walk) runs without the mutex held.
    std::uint64_t fresh=0;
    const bool probeOk=statvfsFreeBytes(m_store,scope,fresh);
    // Substitute the cached value ONLY on probe failure; a successful zero
    // (genuinely full card) is cached and returned as zero so makePlan()
    // still reports "Not enough space".
    const std::uint64_t selected=selectCachedFreeBytes(cached,fresh,probeOk);
    // Cache the decision even when it reuses the old value, so a persistently
    // failing statvfs still only retries after the TTL instead of every poll.
    {std::lock_guard<std::mutex>l(m_mutex);m_freeBytesCached=selected;m_freeBytesCachedAtMs=TelemetryClock::monotonicUs()/1000;}
    return selected;
}
DownloadSnapshot DownloadManager::snapshot()const{std::vector<DownloadItem> items;bool playback=false;{std::lock_guard<std::mutex>l(m_mutex);items=m_items;playback=m_playback;
            }DownloadSnapshot x;x.items=std::move(items);x.freeBytes=freeBytes();x.playbackActive=playback;
        for(auto&i:x.items){if(i.state!=DownloadState::Complete&&i.state!=DownloadState::LocalOnly&&i.state!=DownloadState::UpdateAvailable){
                x.reservedBytes=saturatingAdd(x.reservedBytes,queueRemainingBytes(i));
                }else x.localBytes=saturatingAdd(x.localBytes,i.hlsStorage?i.downloadedBytes:(i.expectedSize?i.expectedSize:i.downloadedBytes));}return x;}
bool DownloadManager::updateRecentSpeed(RecentSpeedSample &sample,std::uint64_t downloaded,std::uint64_t now,std::uint64_t &bytesPerSec){
    static constexpr std::uint64_t WINDOW_MS=1500;
    // HLS inter-segment gaps (server-side transcoding) are normal and can
    // last several seconds.  Only zero the displayed rate when the gap is
    // long enough to indicate a real stall.
    static constexpr std::uint64_t STALL_GAP_MS=5000;
    if(sample.samples.empty()||downloaded<sample.downloadedBytes||now<sample.samples.back().first){sample={};sample.downloadedBytes=downloaded;sample.lastReceivedMs=now;
            sample.samples.push_back({now,downloaded});return false;}
    if(downloaded==sample.downloadedBytes){
        if(now>=sample.lastReceivedMs+STALL_GAP_MS && bytesPerSec){bytesPerSec=0;return true;}
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
bool DownloadManager::hasComplete(const std::string&id)const{std::lock_guard<std::mutex>l(m_mutex);
        for(auto&i:m_items)if(i.itemId==id&&(i.state==DownloadState::Complete||i.state==DownloadState::LocalOnly||i.state==DownloadState::UpdateAvailable)&&m_store.validateCompletedDownload(m_scope,
        i,nullptr))return true;
        return false;}
}
