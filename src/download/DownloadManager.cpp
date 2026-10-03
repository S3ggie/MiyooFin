#include "DownloadManager.hpp"
#include "DownloadSupport.hpp"
#include "../library/LibraryCoordinator.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include <sys/statvfs.h>
#include <algorithm>

namespace miyoofin {
namespace {

void publishDownloadGauges(const std::vector<DownloadItem>& items, std::size_t plannerQueueDepth)
{
    std::uint32_t active = 0;
    std::uint32_t queued = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::Downloading)
            ++active;
        else if (item.state == DownloadState::Queued)
            ++queued;
    }
    performanceTelemetry().setDownloadGauges(active, queued,
                                             static_cast<std::uint32_t>(plannerQueueDepth));
}

} // namespace

DownloadManager::DownloadManager(const Session& s, const std::string& r) : m_store(r)
{
    configure(s);
    m_persisterThread = std::thread(&DownloadManager::persisterLoop, this);
    m_thread = std::thread(&DownloadManager::worker, this);
    m_planThread = std::thread(&DownloadManager::planner, this);
    m_reconcileThread = std::thread(&DownloadManager::reconciler, this);
}

DownloadManager::~DownloadManager()
{
    {
        // Whole-library write justified: rare lifecycle event (shutdown); every steady-state
        // transition already asked for its own write, this only converges any last state.
        std::lock_guard<std::mutex> lock(m_mutex);
        persistLocked();
    }
    flushPersistence(); // the storage thread drains what is owed, then is stopped
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        if (m_activePlanCancellation)
            m_activePlanCancellation->store(true);
        const auto hierarchyRequest = m_activePlanHierarchyRequest.load(std::memory_order_acquire);
        if (hierarchyRequest != 0 && m_activePlanCoordinator)
            m_activePlanCoordinator->cancelHierarchyRequest(hierarchyRequest);
        for (auto& job : m_planJobs) {
            if (job.cancellation)
                job.cancellation->store(true);
        }
    }
    m_wake.notify_all();
    m_planWake.notify_all();
    m_reconcileWake.notify_all();
    m_persisterWake.notify_all();
    if (m_persisterThread.joinable())
        m_persisterThread.join();
    if (m_thread.joinable())
        m_thread.join();
    if (m_planThread.joinable())
        m_planThread.join();
    if (m_reconcileThread.joinable())
        m_reconcileThread.join();
}

// configure() switches the account and does no disk I/O: the previous account's items are handed
// to the storage thread (written, then forgotten) and the new account's library is read by that
// same thread, after every write and removal already queued (so it sees the disk those leave).
// Until that read is published the snapshot is empty rather than showing another account's items.
void DownloadManager::configure(const Session& s)
{
    const std::string newScope =
        s.valid() ? DownloadStore::scopeKey(s.serverUrl, s.userId) : "anonymous";
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool sameScope = m_configured && newScope == m_scope;
    if (m_activePlanCancellation)
        m_activePlanCancellation->store(true);
    const auto hierarchyRequest = m_activePlanHierarchyRequest.load(std::memory_order_acquire);
    if (hierarchyRequest != 0 && m_activePlanCoordinator)
        m_activePlanCoordinator->cancelHierarchyRequest(hierarchyRequest);
    for (auto& job : m_planJobs) {
        if (job.cancellation)
            job.cancellation->store(true);
    }
    ++m_generation;
    m_session = s;
    m_deleteRequested.clear();
    m_progressSamples.clear();
    if (!sameScope) {
        if (m_configured && !m_items.empty())
            m_storageFinalWrites.push_back({m_scope, m_items, !m_loading});
        m_asyncPersistIds.clear();
        m_rescanIds.clear();
        // Removals already queued (for any account) stay queued and keep their ids busy.
        m_segmentsVerified.clear();
        m_scope = newScope;
        m_items.clear();
        m_indexedIds.clear();
        m_loading = true;
        ++m_loadToken;
        m_storageLoads.clear(); // only the newest request matters
        m_storageLoads.push_back({newScope, m_loadToken});
    }
    m_configured = true;
    if (s.valid() && !m_loading) {
        m_reconcileRequested = true;
        m_startupReconcile = true;
    }
    publishDownloadGauges(m_items, m_planJobs.size());
    if (s.valid() && !m_loading)
        performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile, 1);
    m_wake.notify_all();
    m_reconcileWake.notify_one();
    m_persisterWake.notify_all();
}

// Storage thread: read the library of `scope`, then publish it unless the account changed again.
void DownloadManager::runLoad(const std::string& scope, std::uint64_t token)
{
    if (beforeAsyncWriteForTest)
        beforeAsyncWriteForTest();
    std::vector<DownloadItem> loaded;
    if (!m_store.loadIndex(scope, loaded, nullptr)) {
        loaded.clear();
        std::vector<DownloadItem> rebuilt;
        if (m_store.rebuildIndex(scope, rebuilt, nullptr))
            loaded.swap(rebuilt);
    }
    std::set<std::string> seen;
    loaded.erase(
        std::remove_if(loaded.begin(), loaded.end(),
                       [&](const DownloadItem& i) { return !seen.insert(i.itemId).second; }),
        loaded.end());
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stop || token != m_loadToken || scope != m_scope)
        return; // superseded: the account changed again
    for (auto& item : loaded) {
        if (storageBusyLocked(scope, item.itemId))
            continue; // being removed: the disk copy is going away
        const bool known = std::any_of(m_items.begin(), m_items.end(), [&](const DownloadItem& i) {
            return i.itemId == item.itemId;
        });
        if (known)
            continue; // added again since the switch: the newer in-memory state wins
        // A transfer cannot be running for a library that was just read: a leftover
        // "Downloading" is from before an exit or an account switch, so it waits its turn again.
        if (item.state == DownloadState::Downloading)
            item.state = DownloadState::Queued;
        m_indexedIds.insert(item.itemId);
        m_items.push_back(std::move(item));
    }
    m_loading = false;
    m_rescanAllDue = true;
    m_asyncIndexDue = true; // the index is rewritten once, now that it can list everything
    if (m_session.valid()) {
        m_reconcileRequested = true;
        m_startupReconcile = true;
        performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile, 1);
    }
    publishDownloadGauges(m_items, m_planJobs.size());
    m_wake.notify_all();
    m_reconcileWake.notify_one();
    m_persisterWake.notify_all();
}

// Drops removed ids from a scope's on-disk index (a scope that is not, or not yet, fully live).
void DownloadManager::patchIndexRemoving(const std::string& scope, const std::set<std::string>& ids)
{
    std::vector<DownloadItem> index;
    if (!m_store.loadIndex(scope, index, nullptr))
        return;
    const auto before = index.size();
    index.erase(std::remove_if(index.begin(), index.end(),
                               [&](const DownloadItem& i) { return ids.count(i.itemId) != 0; }),
                index.end());
    if (index.size() != before)
        m_store.saveIndex(scope, index, nullptr);
}

void DownloadManager::requestAsyncPersistLocked(const std::string& itemId)
{
    m_asyncPersistIds.insert(itemId);
    m_persisterWake.notify_all();
}

void DownloadManager::requestIndexLocked()
{
    m_asyncIndexDue = true;
    m_persisterWake.notify_all();
}

void DownloadManager::requestRescanLocked(const std::string& itemId)
{
    m_rescanIds.insert(itemId);
    m_segmentsVerified.erase(itemId);
    m_persisterWake.notify_all();
}

void DownloadManager::requestRemoveLocked(const std::string& itemId, bool wholeItem)
{
    if (wholeItem)
        m_asyncPersistIds.erase(itemId); // the removal supersedes any write still owed
    m_segmentsVerified.erase(itemId);
    m_storageRemovals.push_back({m_scope, itemId, wholeItem});
    ++m_storageBusy[busyKey(m_scope, itemId)];
    m_persisterWake.notify_all();
}

// Kept for the existing call sites: the writes are requests now.
void DownloadManager::saveIndexLocked()
{
    requestIndexLocked();
}

void DownloadManager::persistLocked()
{
    for (const auto& i : m_items)
        m_asyncPersistIds.insert(i.itemId);
    m_asyncIndexDue = true;
    m_persisterWake.notify_all();
}

void DownloadManager::persistItemLocked(const std::string& id)
{
    requestAsyncPersistLocked(id);
}

bool DownloadManager::storageHasWorkLocked() const
{
    return !m_asyncPersistIds.empty() || m_asyncIndexDue || !m_storageRemovals.empty() ||
           !m_storageFinalWrites.empty() || !m_rescanIds.empty() || m_rescanAllDue ||
           !m_storageLoads.empty();
}

void DownloadManager::flushPersistence()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_persisterWake.wait(
        lock, [this] { return m_stop || (!storageHasWorkLocked() && !m_persisterBusy); });
}

// Writes `itemId`'s manifest from a copy taken under the lock, with the lock released for the
// disk I/O, then checks the live item did not change meanwhile (or get overwritten by a
// synchronous writer's newer state): if it did, the newer state is written too. The file therefore
// always converges to the live state, and an older copy can never be what stays on disk.
bool DownloadManager::persistManifestAsync(const std::string& scope, std::uint64_t,
                                           const std::string& itemId)
{
    bool newlyIndexed = false;
    for (int round = 0; round < 8; ++round) {
        DownloadItem copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop || scope != m_scope)
                return newlyIndexed;
            auto it = std::find_if(m_items.begin(), m_items.end(),
                                   [&](const DownloadItem& i) { return i.itemId == itemId; });
            if (it == m_items.end())
                return newlyIndexed; // erased meanwhile: nothing to write, nothing to resurrect
            copy = *it;
        }
        if (beforeAsyncWriteForTest)
            beforeAsyncWriteForTest();
        m_store.ensureHlsDirectories(scope, itemId);
        const bool ok = m_store.saveManifest(scope, copy, nullptr);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (scope != m_scope)
            return newlyIndexed;
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const DownloadItem& i) { return i.itemId == itemId; });
        if (it == m_items.end())
            return newlyIndexed;
        if (ok && m_indexedIds.insert(itemId).second)
            newlyIndexed = true;
        if (DownloadStore::manifestText(*it) == DownloadStore::manifestText(copy))
            return newlyIndexed; // what is on disk is the live state
    }
    return newlyIndexed;
}

void DownloadManager::persistIndexAsync(const std::string& scope, std::uint64_t)
{
    for (int round = 0; round < 8; ++round) {
        std::vector<DownloadItem> indexed;
        std::set<std::string> idsWritten;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop || scope != m_scope)
                return;
            if (m_loading)
                return; // the index would drop what has not been read yet; the load rewrites it
            for (const auto& i : m_items)
                if (m_indexedIds.count(i.itemId)) {
                    indexed.push_back(i);
                    idsWritten.insert(i.itemId);
                }
        }
        if (beforeAsyncWriteForTest)
            beforeAsyncWriteForTest();
        m_store.saveIndex(scope, indexed, nullptr);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (scope != m_scope)
            return;
        std::set<std::string> now;
        for (const auto& i : m_items)
            if (m_indexedIds.count(i.itemId))
                now.insert(i.itemId);
        if (now == idsWritten)
            return; // the index lists exactly the live set
    }
}

// Removes an item's files (or only its downloaded bytes) on the storage thread, then lets the
// transfer worker start that id again if it was added back meanwhile.
void DownloadManager::runRemoval(const std::string& scope, const std::string& itemId,
                                 bool wholeItem)
{
    if (beforeAsyncWriteForTest)
        beforeAsyncWriteForTest();
    std::string error;
    const bool ok = wholeItem ? m_store.removeItem(scope, itemId, &error)
                              : m_store.removePartialBytes(scope, itemId, &error);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto busy = m_storageBusy.find(busyKey(scope, itemId));
    if (busy != m_storageBusy.end() && --busy->second <= 0)
        m_storageBusy.erase(busy);
    if (!ok)
        std::printf("[Download] storage: could not remove %s%s: %s\n",
                    wholeItem ? "" : "old bytes of ", itemId.c_str(), error.c_str());
    if (scope == m_scope) {
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const DownloadItem& i) { return i.itemId == itemId; });
        if (it != m_items.end()) {
            // Added back (or re-downloading) while the old files were going: it needs a manifest
            // again, and an old copy that could not be cleared must not be built on.
            if (!ok && !wholeItem && it->state == DownloadState::Queued) {
                it->state = DownloadState::Failed;
                it->lastError = "Could not clear the old copy";
            }
            m_asyncPersistIds.insert(itemId);
            m_asyncIndexDue = true;
        }
    }
    m_wake.notify_all();
    m_persisterWake.notify_all();
}

// Re-reads one item's segment files on a copy and merges what it learned into the live item,
// but only where the live item has not changed in the meantime.
void DownloadManager::rescanItemAsync(const std::string& scope, std::uint64_t generation,
                                      const std::string& itemId)
{
    DownloadItem copy;
    DownloadState before;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop || generation != m_generation || scope != m_scope || storageBusyLocked(itemId))
            return;
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const DownloadItem& i) { return i.itemId == itemId; });
        if (it == m_items.end() || it->state == DownloadState::Downloading)
            return; // a transfer in progress owns its counters
        copy = *it;
        before = it->state;
    }
    if (beforeAsyncWriteForTest)
        beforeAsyncWriteForTest();
    m_store.reconcileInMemory(scope, copy, nullptr);
    const bool structurallyComplete =
        copy.hlsStorage ? m_store.validateCompletedDownload(scope, copy, nullptr) : true;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (generation != m_generation || scope != m_scope || storageBusyLocked(itemId))
        return;
    auto it = std::find_if(m_items.begin(), m_items.end(),
                           [&](const DownloadItem& i) { return i.itemId == itemId; });
    if (it == m_items.end() || it->state == DownloadState::Downloading)
        return;
    m_segmentsVerified[itemId] = structurallyComplete;
    bool changed = it->downloadedBytes != copy.downloadedBytes;
    it->downloadedBytes = copy.downloadedBytes;
    if (it->state == before && copy.state != before) {
        it->state = copy.state;
        changed = true;
    }
    if (changed) {
        m_asyncPersistIds.insert(itemId);
        publishDownloadGauges(m_items, m_planJobs.size());
        m_wake.notify_all();
    }
}

void DownloadManager::persisterLoop()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_persisterWake.wait(lock, [this] { return m_stop || storageHasWorkLocked(); });
        if (m_stop)
            return;
        std::vector<StorageFinalWrite> finals;
        finals.swap(m_storageFinalWrites);
        std::deque<StorageRemoval> removals;
        removals.swap(m_storageRemovals);
        std::set<std::string> ids;
        ids.swap(m_asyncPersistIds);
        std::set<std::string> rescans;
        rescans.swap(m_rescanIds);
        if (m_rescanAllDue) {
            for (const auto& i : m_items)
                rescans.insert(i.itemId);
            m_rescanAllDue = false;
        }
        bool indexDue = m_asyncIndexDue;
        m_asyncIndexDue = false;
        const std::string scope = m_scope;
        const std::uint64_t generation = m_generation;
        m_persisterBusy = true;
        lock.unlock();
        // 1. A previous account's items, written out whole before they are forgotten.
        for (const StorageFinalWrite& f : finals) {
            std::vector<DownloadItem> indexed;
            if (!f.complete)
                m_store.loadIndex(f.scope, indexed, nullptr); // what was never read stays listed
            for (const DownloadItem& i : f.items) {
                if (beforeAsyncWriteForTest)
                    beforeAsyncWriteForTest();
                if (m_store.saveManifest(f.scope, i, nullptr)) {
                    indexed.erase(
                        std::remove_if(indexed.begin(), indexed.end(),
                                       [&](const DownloadItem& x) { return x.itemId == i.itemId; }),
                        indexed.end());
                    indexed.push_back(i);
                }
            }
            m_store.saveIndex(f.scope, indexed, nullptr);
        }
        // 2. Removals, in the order they were asked for; each scope's index is then corrected.
        std::map<std::string, std::set<std::string>> removedByScope;
        for (const StorageRemoval& r : removals) {
            runRemoval(r.scope, r.id, r.wholeItem);
            if (r.wholeItem)
                removedByScope[r.scope].insert(r.id);
        }
        for (const auto& entry : removedByScope) {
            bool live;
            {
                std::lock_guard<std::mutex> relock(m_mutex);
                live = entry.first == m_scope && !m_loading;
            }
            if (live)
                indexDue = true;
            else
                patchIndexRemoving(entry.first, entry.second);
        }
        // 2b. Libraries to read (after the writes and removals above, which they must observe).
        {
            std::vector<StorageLoad> loads;
            {
                std::lock_guard<std::mutex> relock(m_mutex);
                loads.swap(m_storageLoads);
            }
            for (const StorageLoad& l : loads)
                runLoad(l.scope, l.token);
        }
        // 3. Re-reads of segment files.
        for (const std::string& id : rescans)
            rescanItemAsync(scope, generation, id);
        // 4. Manifests (writes after removals, so a re-added id is written to a clean slate).
        bool newlyIndexed = false;
        const std::set<std::string>& toWrite = ids; // later requests get their own cycle
        for (const std::string& id : toWrite)
            newlyIndexed = persistManifestAsync(scope, generation, id) || newlyIndexed;
        // 5. The index, when ids came or went.
        if (indexDue || newlyIndexed)
            persistIndexAsync(scope, generation);
        lock.lock();
        m_persisterBusy = false;
        m_persisterWake.notify_all();
    }
}

void DownloadManager::setPlaybackActive(bool v)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_playback = v;
        // Multi-item transition, but still per-item: each affected item's
        // manifest is written individually under the lock (never the whole
        // library), so an unrelated item's on-disk state is untouched.
        for (auto& i : m_items) {
            if (v && i.state == DownloadState::Downloading) {
                i.state = stateAfterInterrupt(DownloadInterrupt::Playback);
                i.recentBytesPerSec = 0;
                m_progressSamples.erase(i.itemId);
                requestAsyncPersistLocked(i.itemId);
            } else if (!v && i.state == DownloadState::PausedForPlayback) {
                i.state = DownloadState::Queued;
                requestAsyncPersistLocked(i.itemId);
            }
        }
        publishDownloadGauges(m_items, m_planJobs.size());
    }
    m_wake.notify_all();
}

void DownloadManager::enqueue(const DownloadItem& item)
{
    enqueue(std::vector<DownloadItem>{item});
}

void DownloadManager::enqueue(const std::vector<DownloadItem>& incoming)
{
    // This is called directly from screen input handling.  Keep it strictly
    // in-memory: manifests, index persistence, and directory creation belong
    // to worker(), before it starts the item's transfer.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& in : incoming) {
            auto p = std::find_if(m_items.begin(), m_items.end(),
                                  [&](const DownloadItem& i) { return i.itemId == in.itemId; });
            if (p != m_items.end())
                continue;
            DownloadItem i = in;
            if (i.chunkSize == 0)
                i.chunkSize = DOWNLOAD_CHUNK_SIZE;
            i.hlsStorage = true;
            i.hlsProfile = HLS_PROFILE_NAME;
            estimateHlsBytes(i.runtimeTicks, i.expectedSize);
            i.recentBytesPerSec = 0;
            i.state = DownloadState::Queued;
            m_progressSamples.erase(i.itemId);
            m_items.push_back(std::move(i));
            requestAsyncPersistLocked(m_items.back().itemId);
        }
        m_reconcileRequested = true;
        performanceTelemetry().setWorkerQueueDepth(WorkerId::DownloadReconcile, 1);
        publishDownloadGauges(m_items, m_planJobs.size());
    }
    m_wake.notify_one();
    m_reconcileWake.notify_one();
}

void DownloadManager::pause(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& i : m_items) {
        if (i.itemId == id && i.state != DownloadState::Complete) {
            i.state = stateAfterInterrupt(DownloadInterrupt::UserPause);
            i.recentBytesPerSec = 0;
            m_progressSamples.erase(id);
        }
    }
    // The transition is in memory now; the storage thread writes the manifest and then the
    // index (a durable manifest always travels with its index write, as before), without the
    // caller or the snapshot lock waiting for the disk.
    requestAsyncPersistLocked(id);
    publishDownloadGauges(m_items, m_planJobs.size());
    m_wake.notify_all();
}

void DownloadManager::resume(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& i : m_items) {
        if (i.itemId == id && i.state != DownloadState::Complete)
            i.state = DownloadState::Queued;
    }
    requestAsyncPersistLocked(id); // same as pause(): memory now, disk via the storage thread
    publishDownloadGauges(m_items, m_planJobs.size());
    m_wake.notify_one();
}

void DownloadManager::retry(const std::string& id)
{
    resume(id);
}

bool DownloadManager::redownload(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& i : m_items) {
        if (i.itemId == id && i.state == DownloadState::UpdateAvailable &&
            !i.availableMediaSourceId.empty() && (i.hlsStorage || i.availableSize)) {
            // The old bytes go on the storage thread; the transfer worker waits (m_storageBusy)
            // until they are gone before it starts this id again.
            requestRemoveLocked(id, false);
            i.mediaSourceId = i.availableMediaSourceId;
            i.sourceEtag = i.availableSourceEtag;
            if (i.hlsStorage)
                estimateHlsBytes(i.runtimeTicks, i.expectedSize);
            else
                i.expectedSize = i.availableSize;
            i.availableMediaSourceId.clear();
            i.availableSourceEtag.clear();
            i.availableSize = 0;
            i.downloadedBytes = 0;
            i.recentBytesPerSec = 0;
            m_progressSamples.erase(id);
            i.state = DownloadState::Queued;
            i.updateAvailable = false;
            i.localOnly = false;
            persistItemLocked(i.itemId);
            publishDownloadGauges(m_items, m_planJobs.size());
            m_wake.notify_one();
            return true;
        }
    }
    return false;
}

bool DownloadManager::erase(const std::string& id, std::string* e)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto p = std::find_if(m_items.begin(), m_items.end(),
                          [&](const DownloadItem& i) { return i.itemId == id; });
    if (p == m_items.end())
        return false;
    if (p->state == DownloadState::Downloading) {
        m_deleteRequested.insert(id);
        p->state = DownloadState::Paused;
        p->recentBytesPerSec = 0;
        m_progressSamples.erase(id);
        persistItemLocked(id);
        publishDownloadGauges(m_items, m_planJobs.size());
        m_wake.notify_all();
        return true;
    }
    // Gone from memory (and so from every snapshot) at once; its files, manifest and index entry
    // are removed by the storage thread, which also keeps the id from being started or written
    // in the meantime. A removal that fails there is logged; it cannot be reported from here.
    (void)e;
    m_deleteRequested.erase(id);
    m_items.erase(p);
    m_progressSamples.erase(id);
    m_indexedIds.erase(id);
    requestRemoveLocked(id, true);
    requestIndexLocked();
    publishDownloadGauges(m_items, m_planJobs.size());
    return true;
}

bool DownloadManager::statvfsFreeBytes(const DownloadStore& store, const std::string& scope,
                                       std::uint64_t& out)
{
    struct statvfs s
    {};
    std::string path = store.scopePath(scope);
    while (!path.empty()) {
        if (!statvfs(path.c_str(), &s)) {
            out = (std::uint64_t)s.f_bavail * (std::uint64_t)s.f_frsize;
            return true;
        }
        std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos)
            break;
        path.resize(slash);
    }
    return false;
}

std::uint64_t DownloadManager::freeBytes() const
{
    const std::uint64_t nowMs = TelemetryClock::monotonicUs() / 1000;
    std::string scope;
    std::uint64_t cached = 0;
    std::uint64_t cachedAt = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scope = m_scope;
        cached = m_freeBytesCached;
        cachedAt = m_freeBytesCachedAtMs;
        if (!freeSpaceCacheExpired(nowMs, cachedAt))
            return cached;
    }
    // statvfs (with its parent-dir walk) runs without the mutex held.
    std::uint64_t fresh = 0;
    const bool probeOk = statvfsFreeBytes(m_store, scope, fresh);
    // Substitute the cached value ONLY on probe failure; a successful zero
    // (genuinely full card) is cached and returned as zero so makePlan()
    // still reports "Not enough space".
    const std::uint64_t selected = selectCachedFreeBytes(cached, fresh, probeOk);
    // Cache the decision even when it reuses the old value, so a persistently
    // failing statvfs still only retries after the TTL instead of every poll.
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_freeBytesCached = selected;
        m_freeBytesCachedAtMs = TelemetryClock::monotonicUs() / 1000;
    }
    return selected;
}

DownloadSnapshot DownloadManager::snapshot() const
{
    std::vector<DownloadItem> items;
    bool playback = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        items = m_items;
        playback = m_playback;
    }
    DownloadSnapshot x;
    x.items = std::move(items);
    x.freeBytes = freeBytes();
    x.playbackActive = playback;
    for (auto& i : x.items) {
        if (i.state != DownloadState::Complete && i.state != DownloadState::LocalOnly &&
            i.state != DownloadState::UpdateAvailable) {
            x.reservedBytes = saturatingAdd(x.reservedBytes, queueRemainingBytes(i));
        } else {
            x.localBytes = saturatingAdd(
                x.localBytes, i.hlsStorage ? i.downloadedBytes
                                           : (i.expectedSize ? i.expectedSize : i.downloadedBytes));
        }
    }
    return x;
}

bool DownloadManager::updateRecentSpeed(RecentSpeedSample& sample, std::uint64_t downloaded,
                                        std::uint64_t now, std::uint64_t& bytesPerSec)
{
    static constexpr std::uint64_t WINDOW_MS = 1500;
    // HLS inter-segment gaps (server-side transcoding) are normal and can
    // last several seconds.  Only zero the displayed rate when the gap is
    // long enough to indicate a real stall.
    static constexpr std::uint64_t STALL_GAP_MS = 5000;
    if (sample.samples.empty() || downloaded < sample.downloadedBytes ||
        now < sample.samples.back().first) {
        sample = {};
        sample.downloadedBytes = downloaded;
        sample.lastReceivedMs = now;
        sample.samples.push_back({now, downloaded});
        return false;
    }
    if (downloaded == sample.downloadedBytes) {
        if (now >= sample.lastReceivedMs + STALL_GAP_MS && bytesPerSec) {
            bytesPerSec = 0;
            return true;
        }
        return false;
    }
    sample.downloadedBytes = downloaded;
    sample.lastReceivedMs = now;
    sample.samples.push_back({now, downloaded});
    while (sample.samples.size() > 1 && now - sample.samples[1].first > WINDOW_MS)
        sample.samples.pop_front();
    const auto& first = sample.samples.front();
    if (now <= first.first)
        return false;
    const std::uint64_t rate = (downloaded - first.second) * 1000 / (now - first.first);
    if (rate == bytesPerSec)
        return false;
    bytesPerSec = rate;
    return true;
}

bool DownloadManager::hasComplete(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& i : m_items) {
        if (i.itemId == id &&
            (i.state == DownloadState::Complete || i.state == DownloadState::LocalOnly ||
             i.state == DownloadState::UpdateAvailable) &&
            !storageBusyLocked(id)) {
            // Memory only (callers are on the UI thread): the manifest state is trusted until the
            // storage thread's rescan has checked the files and recorded a failure.
            auto v = m_segmentsVerified.find(id);
            return v == m_segmentsVerified.end() || v->second;
        }
    }
    return false;
}

} // namespace miyoofin
