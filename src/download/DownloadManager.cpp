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
    // One attempt at everything owed, then stop: work that still fails is reported, never waited
    // for (a permanently failing disk must not hang shutdown).
    if (!flushPersistence())
        std::printf("[Download] storage: shutting down with unwritten state\n");
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
        m_scopeFresh = false;
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
// A read that fails is NOT an empty library: nothing is published, the account stays
// non-authoritative (no index is written for it) and the load stays owed and is retried.
bool DownloadManager::runLoad(const std::string& scope, std::uint64_t token)
{
    // A final write of this account that has not reached the disk yet (its write failed and is
    // being retried) is newer than anything on the disk: it becomes the library, not the disk copy.
    // It is only consumed once the load is published.
    std::vector<DownloadItem> pending;
    bool havePending = false, pendingComplete = true;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop || token != m_loadToken || scope != m_scope)
            return true; // superseded
        for (const auto& f : m_storageFinalWrites) {
            if (f.scope == scope) {
                havePending = true;
                pendingComplete = pendingComplete && f.complete;
                pending = f.items; // later entries are newer
            }
        }
    }
    callAsyncWriteHook();
    std::vector<DownloadItem> loaded;
    bool readFresh = true; // nothing read from disk (an owed final write) counts as unverified
    if (!havePending || !pendingComplete) {
        std::string why;
        const LibraryStatus status = m_store.readLibrary(scope, loaded, &why);
        readFresh = status == LibraryStatus::NewScope;
        if (status == LibraryStatus::Unavailable) {
            std::printf("[Download] storage: could not read the downloads library: %s\n",
                        why.c_str());
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_stop && token == m_loadToken && scope == m_scope && m_storageLoads.empty())
                m_storageLoads.push_back({scope, token});
            return false;
        }
    }
    std::set<std::string> seen;
    std::set<std::string> notDurable;
    std::vector<DownloadItem> merged;
    for (const auto& item : pending) {
        if (seen.insert(item.itemId).second) {
            notDurable.insert(item.itemId);
            merged.push_back(item);
        }
    }
    for (auto& item : loaded)
        if (seen.insert(item.itemId).second)
            merged.push_back(std::move(item));
    loaded.swap(merged);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stop || token != m_loadToken || scope != m_scope)
        return true; // superseded: the account changed again (its final writes are still owed)
    m_storageFinalWrites.erase(
        std::remove_if(m_storageFinalWrites.begin(), m_storageFinalWrites.end(),
                       [&](const StorageFinalWrite& f) { return f.scope == scope; }),
        m_storageFinalWrites.end());
    mergeLoadedLocked(scope, loaded, notDurable);
    m_loading = false;
    m_scopeFresh = readFresh;
    m_lateScanOwed = false; // this read saw the whole library
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
    return true;
}

// Adds what a read found to the live state (caller holds m_mutex): ids being removed are skipped
// (the disk copy is going away) and so are ids already live (the newer in-memory state wins).
void DownloadManager::mergeLoadedLocked(const std::string& scope, std::vector<DownloadItem>& loaded,
                                        const std::set<std::string>& notDurable)
{
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
        if (notDurable.count(item.itemId))
            m_asyncPersistIds.insert(item.itemId); // becomes indexed once its write succeeds
        else
            m_indexedIds.insert(item.itemId);
        m_items.push_back(std::move(item));
    }
}

// Drops removed ids from a scope's on-disk index (a scope that is not, or not yet, fully live).
// Returns false when the index could not be read or written (the patch stays owed).
bool DownloadManager::patchIndexRemoving(const std::string& scope, const std::set<std::string>& ids)
{
    std::vector<DownloadItem> index;
    switch (m_store.readLibrary(scope, index, nullptr)) {
    case LibraryStatus::Unavailable:
        return false; // could not look: the patch stays owed, nothing is retired or rewritten
    case LibraryStatus::NewScope:
    case LibraryStatus::Rebuilt:
        return true; // nothing indexed lists these ids (a rebuilt index never names a removed one)
    case LibraryStatus::Loaded:
        break;
    }
    const auto before = index.size();
    index.erase(std::remove_if(index.begin(), index.end(),
                               [&](const DownloadItem& i) { return ids.count(i.itemId) != 0; }),
                index.end());
    return index.size() == before || m_store.saveIndex(scope, index, nullptr);
}

// Asks the storage thread to read the current account's library again and merge it in (the
// account stays usable meanwhile; index writes wait until the merge is published).
void DownloadManager::requestReloadLocked(const std::string& scope)
{
    if (scope != m_scope)
        return;
    m_loading = true;
    m_scopeFresh = false;
    ++m_loadToken;
    m_storageLoads.clear();
    m_storageLoads.push_back({scope, m_loadToken});
    m_asyncIndexDue = true;
    m_persisterWake.notify_all();
}

void DownloadManager::requestAsyncPersistLocked(const std::string& itemId)
{
    m_retryNotBefore = {}; // fresh work is not held back by an earlier failure's backoff
    m_asyncPersistIds.insert(itemId);
    m_persisterWake.notify_all();
}

void DownloadManager::requestIndexLocked()
{
    m_retryNotBefore = {}; // fresh work is not held back by an earlier failure's backoff
    m_asyncIndexDue = true;
    m_persisterWake.notify_all();
}

void DownloadManager::requestRescanLocked(const std::string& itemId)
{
    m_retryNotBefore = {}; // fresh work is not held back by an earlier failure's backoff
    m_rescanIds.insert(itemId);
    m_segmentsVerified.erase(itemId);
    m_persisterWake.notify_all();
}

void DownloadManager::requestRemoveLocked(const std::string& itemId, bool wholeItem)
{
    m_retryNotBefore = {}; // fresh work is not held back by an earlier failure's backoff
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
    m_retryNotBefore = {}; // fresh work is not held back by an earlier failure's backoff
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
           !m_storageLoads.empty() || !m_indexPatches.empty();
}

void DownloadManager::setAsyncWriteHookForTest(std::function<void()> hook)
{
    std::unique_lock<std::mutex> lock(m_hookMutex);
    m_hook = hook ? std::make_shared<std::function<void()>>(std::move(hook)) : nullptr;
    if (!m_hook)
        m_hookDrained.wait(lock, [this] { return m_hookInflight == 0; });
}

void DownloadManager::setIndexGapHookForTest(std::function<void()> hook)
{
    std::unique_lock<std::mutex> lock(m_hookMutex);
    m_gapHook = hook ? std::make_shared<std::function<void()>>(std::move(hook)) : nullptr;
    if (!m_gapHook)
        m_hookDrained.wait(lock, [this] { return m_hookInflight == 0; });
}

void DownloadManager::callAsyncWriteHook(bool gap)
{
    std::shared_ptr<std::function<void()>> hook;
    {
        std::lock_guard<std::mutex> lock(m_hookMutex);
        hook = gap ? m_gapHook : m_hook;
        if (!hook)
            return;
        ++m_hookInflight;
    }
    (*hook)(); // outside the lock; the copy keeps the callable alive
    std::lock_guard<std::mutex> lock(m_hookMutex);
    if (--m_hookInflight == 0)
        m_hookDrained.notify_all();
}

std::string DownloadManager::storageError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_storageError;
}

bool DownloadManager::flushPersistence()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!storageHasWorkLocked() && !m_persisterBusy)
        return true;
    // Returns when nothing is owed, or when a pass that began after this call ended with
    // failures (work that fails stays owed; it is not waited for). A clean pass that leaves
    // follow-up work (a library just loaded asks for an index write) is followed by the next.
    const std::uint64_t target = m_passSeq + (m_persisterBusy ? 2 : 1);
    m_kick = true;
    m_persisterWake.notify_all();
    m_persisterWake.wait(lock, [&] {
        return m_stop || (m_passSeq >= target && m_lastPassFailed) ||
               (!storageHasWorkLocked() && !m_persisterBusy);
    });
    return !m_stop && !storageHasWorkLocked() && !m_persisterBusy;
}

// Writes `itemId`'s manifest from a copy taken under the lock, with the lock released for the
// disk I/O, then checks the live item did not change meanwhile (or get overwritten by a
// synchronous writer's newer state): if it did, the newer state is written too. The file therefore
// always converges to the live state, and an older copy can never be what stays on disk.
// A write that did not succeed is reported (failed) and the id stays owed; if the convergence
// rounds run out the id is simply asked for again.
DownloadManager::PersistResult DownloadManager::persistManifestAsync(const std::string& scope,
                                                                     std::uint64_t,
                                                                     const std::string& itemId)
{
    PersistResult result;
    for (int round = 0; round < 8; ++round) {
        DownloadItem copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop || scope != m_scope)
                return result;
            auto it = std::find_if(m_items.begin(), m_items.end(),
                                   [&](const DownloadItem& i) { return i.itemId == itemId; });
            if (it == m_items.end())
                return result; // erased meanwhile: nothing to write, nothing to resurrect
            copy = *it;
        }
        callAsyncWriteHook();
        m_store.ensureHlsDirectories(scope, itemId);
        const bool ok = m_store.saveManifest(scope, copy, nullptr);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (scope != m_scope)
            return result; // the account was left: its final write carries the state
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const DownloadItem& i) { return i.itemId == itemId; });
        if (it == m_items.end())
            return result;
        if (!ok) {
            result.failed = true; // acknowledged as not written: still owed
            return result;
        }
        if (m_indexedIds.insert(itemId).second)
            result.newlyIndexed = true;
        if (DownloadStore::manifestText(*it) == DownloadStore::manifestText(copy))
            return result; // what is on disk is the live state
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_asyncPersistIds.insert(itemId); // still changing: converge on a later pass
    return result;
}

bool DownloadManager::persistIndexAsync(const std::string& scope, std::uint64_t)
{
    for (int round = 0; round < 8; ++round) {
        std::vector<DownloadItem> indexed;
        std::set<std::string> idsWritten;
        bool fresh = false, lateScan = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop || scope != m_scope)
                return true;
            if (m_loading)
                return true; // the index would drop what has not been read yet; the load rewrites
                             // it
            fresh = m_scopeFresh;
            lateScan = m_lateScanOwed;
            for (const auto& i : m_items)
                if (m_indexedIds.count(i.itemId)) {
                    indexed.push_back(i);
                    idsWritten.insert(i.itemId);
                }
        }
        if (fresh && indexed.empty())
            return true; // a library read as "nothing there" with nothing to list needs no index
                         // yet
        callAsyncWriteHook();
        if (lateScan) {
            std::vector<DownloadItem> late;
            if (m_store.scanManifests(scope, late, nullptr) == LibraryStatus::Unavailable)
                return false; // could not look: stays owed, the index is not rewritten meanwhile
            std::lock_guard<std::mutex> lock(m_mutex);
            if (scope != m_scope)
                return true;
            mergeLoadedLocked(scope, late, {});
            m_lateScanOwed = false;
            continue; // the index is built again from the merged set
        }
        bool ok;
        if (fresh) {
            // The library was read as "nothing there". If storage has come back with one since,
            // nothing is replaced: it is read again and merged (see runLoad), and only an index
            // that did not exist is ever created here.
            std::vector<DownloadItem> found;
            const LibraryStatus st = m_store.readLibrary(scope, found, nullptr);
            if (st == LibraryStatus::Unavailable)
                return false;
            bool foreign = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (scope != m_scope)
                    return true;
                for (const auto& f : found) {
                    const bool ours =
                        std::any_of(m_items.begin(), m_items.end(),
                                    [&](const DownloadItem& i) { return i.itemId == f.itemId; });
                    if (!ours && !storageBusyLocked(scope, f.itemId))
                        foreign = true;
                }
            }
            callAsyncWriteHook(true);
            const DownloadStore::IndexCreate made =
                (st == LibraryStatus::Loaded || foreign)
                    ? DownloadStore::IndexCreate::Exists
                    : m_store.createIndexExclusive(scope, indexed, nullptr);
            if (made == DownloadStore::IndexCreate::Failed)
                return false;
            if (made == DownloadStore::IndexCreate::Exists) {
                std::lock_guard<std::mutex> lock(m_mutex);
                requestReloadLocked(scope);
                return true;
            }
            ok = true;
            // Manifests may have returned between the scan above and the create, and the index
            // just made lists only ours: the next pass scans for them (the index is not trusted
            // for that) and merges them before writing the index again.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (scope == m_scope) {
                m_scopeFresh = false; // established: ordinary replacing writes from here on
                m_lateScanOwed = true;
                m_asyncIndexDue = true;
            }
        } else {
            ok = m_store.saveIndex(scope, indexed, nullptr);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (scope != m_scope)
            return true;
        if (!ok)
            return false; // not on disk: stays owed
        std::set<std::string> now;
        for (const auto& i : m_items)
            if (m_indexedIds.count(i.itemId))
                now.insert(i.itemId);
        if (now == idsWritten)
            return true; // the index lists exactly the live set
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_asyncIndexDue = true; // the id set kept changing: converge on a later pass
    return true;
}

// Removes an item's files (or only its downloaded bytes) on the storage thread, then lets the
// transfer worker start that id again if it was added back meanwhile.
bool DownloadManager::runRemoval(const std::string& scope, const std::string& itemId,
                                 bool wholeItem)
{
    callAsyncWriteHook();
    std::string error;
    const bool ok = wholeItem ? m_store.removeItem(scope, itemId, &error)
                              : m_store.removePartialBytes(scope, itemId, &error);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!ok) {
        // Stays queued by the caller and keeps the id busy, so nothing is built on (or next to)
        // the old files meanwhile and a deleted item cannot come back at the next load.
        std::printf("[Download] storage: could not remove %s%s: %s\n",
                    wholeItem ? "" : "old bytes of ", itemId.c_str(), error.c_str());
        return false;
    }
    auto busy = m_storageBusy.find(busyKey(scope, itemId));
    if (busy != m_storageBusy.end() && --busy->second <= 0)
        m_storageBusy.erase(busy);
    if (scope == m_scope) {
        auto it = std::find_if(m_items.begin(), m_items.end(),
                               [&](const DownloadItem& i) { return i.itemId == itemId; });
        if (it != m_items.end()) {
            // Added back (or re-downloading) while the old files were going: it needs a manifest
            // again.
            m_asyncPersistIds.insert(itemId);
            m_asyncIndexDue = true;
        }
    }
    m_wake.notify_all();
    m_persisterWake.notify_all();
    return true;
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
    callAsyncWriteHook();
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
        for (;;) {
            if (m_stop)
                return;
            if (!storageHasWorkLocked()) {
                m_persisterWake.wait(lock);
                continue;
            }
            // Owed work that failed waits out its backoff (a timed wait, not polling), unless a
            // flush asks for an immediate attempt or fresh work resets the delay.
            if (m_kick || std::chrono::steady_clock::now() >= m_retryNotBefore)
                break;
            m_persisterWake.wait_until(lock, m_retryNotBefore);
        }
        m_kick = false;
        std::vector<StorageFinalWrite> finals;
        finals.swap(m_storageFinalWrites);
        std::deque<StorageRemoval> removals;
        removals.swap(m_storageRemovals);
        std::map<std::string, std::set<std::string>> patches;
        patches.swap(m_indexPatches);
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
        const unsigned retryBaseMs = m_retryBaseMs;
        m_persisterBusy = true;
        lock.unlock();
        bool failed = false;
        // Whatever fails below is handed back, still owed, when the pass ends.
        std::vector<StorageFinalWrite> retryFinals;
        std::deque<StorageRemoval> retryRemovals;
        std::map<std::string, std::set<std::string>> retryPatches;
        std::set<std::string> retryIds;
        // 1. A previous account's items, written out whole before they are forgotten.
        for (const StorageFinalWrite& f : finals) {
            std::vector<DownloadItem> indexed;
            bool ok = true;
            bool mergedIndex = true;
            if (!f.complete) {
                // What was never read stays listed. If it cannot be read, no index is written.
                mergedIndex =
                    m_store.readLibrary(f.scope, indexed, nullptr) != LibraryStatus::Unavailable;
                if (!mergedIndex)
                    indexed.clear();
            }
            for (const DownloadItem& i : f.items) {
                callAsyncWriteHook();
                if (m_store.saveManifest(f.scope, i, nullptr)) {
                    indexed.erase(
                        std::remove_if(indexed.begin(), indexed.end(),
                                       [&](const DownloadItem& x) { return x.itemId == i.itemId; }),
                        indexed.end());
                    indexed.push_back(i);
                } else {
                    ok = false;
                }
            }
            ok = mergedIndex && m_store.saveIndex(f.scope, indexed, nullptr) && ok;
            if (!ok) {
                failed = true;
                retryFinals.push_back(f); // the whole snapshot is rewritten next time
            }
        }
        // 2. Removals, in the order they were asked for; each scope's index is then corrected.
        std::map<std::string, std::set<std::string>> removedByScope;
        bool blocked = false; // a failed removal holds back later ones for the same scope+id
        std::set<std::string> failedKeys;
        for (const StorageRemoval& r : removals) {
            const std::string key = busyKey(r.scope, r.id);
            if (failedKeys.count(key) || !runRemoval(r.scope, r.id, r.wholeItem)) {
                failedKeys.insert(key);
                retryRemovals.push_back(r);
                failed = blocked = true;
                continue;
            }
            if (r.wholeItem)
                removedByScope[r.scope].insert(r.id);
        }
        for (const auto& entry : removedByScope)
            for (const std::string& id : entry.second)
                patches[entry.first].insert(id);
        for (const auto& entry : patches) {
            bool live;
            {
                std::lock_guard<std::mutex> relock(m_mutex);
                live = entry.first == m_scope && !m_loading;
            }
            if (live) {
                indexDue = true;
            } else if (!patchIndexRemoving(entry.first, entry.second)) {
                failed = true;
                retryPatches[entry.first].insert(entry.second.begin(), entry.second.end());
            }
        }
        (void)blocked;
        // 2b. Libraries to read (after the writes and removals above, which they must observe).
        {
            std::vector<StorageLoad> loads;
            {
                std::lock_guard<std::mutex> relock(m_mutex);
                loads.swap(m_storageLoads);
                // Final writes that failed above are still the newest state of their account: a
                // load of that account (below) must find them, not the older disk copy.
                m_storageFinalWrites.insert(m_storageFinalWrites.begin(), retryFinals.begin(),
                                            retryFinals.end());
                retryFinals.clear();
            }
            for (const StorageLoad& l : loads)
                if (!runLoad(l.scope, l.token))
                    failed = true;
        }
        // 3. Re-reads of segment files.
        for (const std::string& id : rescans)
            rescanItemAsync(scope, generation, id);
        // 4. Manifests (writes after removals, so a re-added id is written to a clean slate).
        bool newlyIndexed = false;
        for (const std::string& id : ids) {
            const PersistResult r = persistManifestAsync(scope, generation, id);
            newlyIndexed = r.newlyIndexed || newlyIndexed;
            if (r.failed) {
                failed = true;
                retryIds.insert(id);
            }
        }
        // 5. The index, when ids came or went (and only once its manifests are down).
        bool indexRetry = false;
        if ((indexDue || newlyIndexed) && !persistIndexAsync(scope, generation)) {
            failed = true;
            indexRetry = true;
        }
        lock.lock();
        for (auto it = retryFinals.rbegin(); it != retryFinals.rend(); ++it)
            m_storageFinalWrites.insert(m_storageFinalWrites.begin(), *it);
        for (auto it = retryRemovals.rbegin(); it != retryRemovals.rend(); ++it)
            m_storageRemovals.push_front(*it);
        for (const auto& entry : retryPatches)
            m_indexPatches[entry.first].insert(entry.second.begin(), entry.second.end());
        for (const std::string& id : retryIds) {
            // Only while the item still exists in the library the write was for.
            if (scope == m_scope)
                m_asyncPersistIds.insert(id);
        }
        if (indexRetry && scope == m_scope)
            m_asyncIndexDue = true;
        if (failed) {
            ++m_retryAttempt;
            const unsigned shift = std::min(m_retryAttempt - 1, 5u);
            const unsigned delayMs = std::min(8000u, retryBaseMs << shift);
            m_retryNotBefore =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(delayMs);
            m_storageError = m_loading ? "Can't read your saved downloads. Check the SD card."
                                       : "Downloads aren't being saved. Check the SD card.";
        } else {
            m_retryAttempt = 0;
            m_retryNotBefore = {};
        }
        if (!storageHasWorkLocked())
            m_storageError.clear();
        m_lastPassFailed = failed;
        ++m_passSeq;
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
    std::string storageErr;
    bool loadingNow = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        items = m_items;
        playback = m_playback;
        storageErr = m_storageError;
        loadingNow = m_loading;
    }
    DownloadSnapshot x;
    x.items = std::move(items);
    x.freeBytes = freeBytes();
    x.playbackActive = playback;
    x.storageError = storageErr;
    x.loading = loadingNow;
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
