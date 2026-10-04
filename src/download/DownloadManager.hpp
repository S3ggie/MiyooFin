#ifndef MIYOOFIN_DOWNLOAD_MANAGER_HPP
#define MIYOOFIN_DOWNLOAD_MANAGER_HPP
#include "DownloadStore.hpp"
#include "../net/Session.hpp"
#include "../data/MediaItem.hpp"
#include <chrono>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <set>
#include <deque>
#include <memory>
namespace miyoofin {
namespace library {
class LibraryQuery;
class LibraryCoordinator;
}
// In-memory-only rolling transfer-rate samples.  Entries are recorded only
// when bytes arrive, so short HLS gaps do not look like zero-speed transfers.
struct RecentSpeedSample
{
    std::deque<std::pair<std::uint64_t, std::uint64_t>> samples;
    std::uint64_t downloadedBytes = 0, lastReceivedMs = 0;
};
class DownloadManager
{
  public:
    explicit DownloadManager(const Session& session = {}, const std::string& root = "downloads");
    ~DownloadManager();
    void setLibraryServices(std::shared_ptr<library::LibraryQuery> libraryQuery,
                            std::shared_ptr<library::LibraryCoordinator> libraryCoordinator)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_libraryQuery = std::move(libraryQuery);
        m_libraryCoordinator = std::move(libraryCoordinator);
    }
    void configure(const Session& session);
    void setPlaybackActive(bool active);
    void enqueue(const DownloadItem& item);
    void enqueue(const std::vector<DownloadItem>& items);
    void pause(const std::string& itemId);
    void resume(const std::string& itemId);
    void retry(const std::string& itemId);
    void requestReconcile();
    bool redownload(const std::string& itemId);
    bool erase(const std::string& itemId, std::string* error = nullptr);
    DownloadSnapshot snapshot() const;
    bool hasComplete(const std::string& itemId) const;
    std::string scope() const
    {
        return m_scope;
    }
    DownloadPlan makePlan(const std::vector<DownloadItem>& items) const;
    /// Preflight PlaybackInfo on the manager-owned background worker.  A plan
    /// id is generation-scoped, so callers may safely discard stale screens.
    std::uint64_t requestPlan(const std::vector<MediaItem>& items);
    /// Expand seasons and episodes, then preflight the unique episodes on the
    /// planner thread.  This is deliberately kept out of screen update/input.
    std::uint64_t requestSeriesPlan(const std::string& seriesId);
    std::uint64_t requestSeasonPlan(const std::string& seriesId, const std::string& seasonId);
    std::uint64_t requestSeriesPlan(const MediaItem& series);
    std::uint64_t requestSeasonPlan(const MediaItem& series, const MediaItem& season);
    DownloadPlanSnapshot planSnapshot(std::uint64_t id) const;
    /// UI-safe non-blocking snapshot.  A busy manager leaves the caller's
    /// previously published state intact instead of stalling a frame.
    bool tryPlanSnapshot(std::uint64_t id, DownloadPlanSnapshot& snapshot) const;
    /// Makes the storage thread attempt everything it owes now (ignoring retry backoff) and waits
    /// for that pass to end. Returns true only if nothing is owed any more: work that failed
    /// stays owed (and is retried with backoff), so a false result means "not on disk".
    /// Used by shutdown and tests; never call from the UI thread.
    bool flushPersistence();
    /// Non-empty while writes or removals are failing and still owed (UI-readable; also in
    /// DownloadSnapshot::storageError).
    std::string storageError() const;
    /// Test seam: runs on the storage thread before each disk operation (write, removal, scan).
    /// Installed and removed under a lock; clearing it waits until no invocation is still running.
    void setAsyncWriteHookForTest(std::function<void()> hook);
    /// Test seam: runs on the storage thread between the first-index scan and its exclusive create.
    void setIndexGapHookForTest(std::function<void()> hook);
    /// Test seam: first retry delay after a failed storage pass (doubles, capped at 8 s).
    void setRetryBaseMsForTest(unsigned ms)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_retryBaseMs = ms;
    }
    static bool acceptsPlanResult(std::uint64_t jobGeneration, std::uint64_t currentGeneration)
    {
        return jobGeneration == currentGeneration;
    }
    // HLS segments are generated on demand: only actual reachability failures
    // wait for a network retry.  Timeouts and HTTP errors are actionable failures.
    static DownloadState hlsSegmentFailureState(long httpStatus, int curlCode);
    static bool hlsSegmentRetryable(long httpStatus, int curlCode);
    static bool hlsSegmentShouldRetry(long httpStatus, int curlCode, unsigned completedAttempts);
    static constexpr unsigned HLS_SEGMENT_ATTEMPTS = 5;
    // Used by libcurl's C callback; it reads only synchronized manager state.
    bool shouldAbort(const std::string& itemId, const std::string& scope,
                     std::uint64_t generation) const;
    void recordProgress(const std::string& itemId, const std::string& scope,
                        std::uint64_t generation, std::uint64_t downloadedBytes,
                        std::uint64_t nowMs, std::uint64_t currentSegmentBytes = 0,
                        std::uint64_t currentSegmentSize = 0);
    // Returns true when the displayed recent rate changes.  A short transfer
    // gap retains the last meaningful rate; an idle period eventually reports 0.
    static bool updateRecentSpeed(RecentSpeedSample& sample, std::uint64_t downloadedBytes,
                                  std::uint64_t nowMs, std::uint64_t& bytesPerSec);

  private:
    void worker();
    void planner();
    void reconciler();
    bool transfer(DownloadItem& item, const Session& session, const std::string& scope,
                  std::uint64_t generation);
    bool waitForHlsSegmentRetry(const std::string& itemId, const std::string& scope,
                                std::uint64_t generation, unsigned seconds);
    void persistLocked();
    void persistItemLocked(const std::string& itemId);
    void saveIndexLocked();
    // Disk work never runs on a UI caller and never while m_mutex (which snapshot() takes) is
    // held. Memory is the authority; every transition changes it first and asks the storage
    // thread, which owns ALL steady-state disk writes, deletions and rescans, to follow:
    //   * manifests and the index are written from copies taken under the lock, and verified
    //     against the live item afterwards, so what stays on disk is always the live state;
    //   * removals (an erased item, a re-download's old bytes) run in order on that same thread,
    //     so a write can never resurrect what a removal deleted, and an id with removal work
    //     outstanding is not started by the transfer worker until it is done (m_storageBusy);
    //   * rescans of segment files (startup, after a failed segment) read a copy and merge the
    //     result into the live item only where it has not changed meanwhile.
    void requestAsyncPersistLocked(const std::string& itemId);
    void requestIndexLocked();
    void requestRemoveLocked(const std::string& itemId, bool wholeItem);
    void requestRescanLocked(const std::string& itemId);
    static std::string busyKey(const std::string& scope, const std::string& itemId)
    {
        return scope + '\n' + itemId;
    }
    bool storageBusyLocked(const std::string& itemId) const
    {
        return m_storageBusy.count(busyKey(m_scope, itemId)) != 0;
    }
    bool storageBusyLocked(const std::string& scope, const std::string& itemId) const
    {
        return m_storageBusy.count(busyKey(scope, itemId)) != 0;
    }
    // Reads a scope's library (index, else rebuilt from manifests) on the storage thread and
    // publishes it if that scope is still the one being waited for.
    bool runLoad(const std::string& scope, std::uint64_t token); // false: read failed, still owed
    bool patchIndexRemoving(const std::string& scope, const std::set<std::string>& ids);
    void persisterLoop();
    bool storageHasWorkLocked() const;
    // Returns true when the id's manifest became durable for the first time (the index must then
    // list it).
    struct PersistResult
    {
        bool newlyIndexed = false;
        bool failed = false; // the write did not reach the disk: the id stays owed
    };
    PersistResult persistManifestAsync(const std::string& scope, std::uint64_t generation,
                                       const std::string& itemId);
    bool persistIndexAsync(const std::string& scope, std::uint64_t generation); // false: owed
    void callAsyncWriteHook(bool gap = false);
    void mergeLoadedLocked(const std::string& scope, std::vector<DownloadItem>& loaded,
                           const std::set<std::string>& notDurable);
    void requestReloadLocked(const std::string& scope);
    void rescanItemAsync(const std::string& scope, std::uint64_t generation,
                         const std::string& itemId);
    bool runRemoval(const std::string& scope, const std::string& itemId, bool wholeItem);
    std::uint64_t freeBytes() const;
    static bool statvfsFreeBytes(const DownloadStore& store, const std::string& scope,
                                 std::uint64_t& out);
    DownloadStore m_store;
    Session m_session;
    std::string m_scope;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake, m_planWake, m_reconcileWake;
    std::thread m_thread, m_planThread, m_reconcileThread, m_persisterThread;
    std::condition_variable m_persisterWake;
    struct StorageRemoval
    {
        std::string scope, id;
        bool wholeItem; // else: only the downloaded bytes (a re-download starts clean)
    };
    struct StorageFinalWrite
    {
        std::string scope;
        std::vector<DownloadItem> items;
        bool complete; // false: the library had not finished loading, so its index is merged
    };
    struct StorageLoad
    {
        std::string scope;
        std::uint64_t token;
    };
    // Failure handling: owed work stays in the sets above and is retried with backoff.
    std::map<std::string, std::set<std::string>> m_indexPatches; // scope -> ids to drop from index
    bool m_kick = false;         // flush asked for an immediate attempt
    std::uint64_t m_passSeq = 0; // storage passes completed
    unsigned m_retryBaseMs = 250;
    bool m_lastPassFailed = false;
    unsigned m_retryAttempt = 0;
    std::chrono::steady_clock::time_point m_retryNotBefore{};
    std::string m_storageError;
    mutable std::mutex m_hookMutex;
    std::condition_variable m_hookDrained;
    std::shared_ptr<std::function<void()>> m_hook;
    std::shared_ptr<std::function<void()>> m_gapHook;
    unsigned m_hookInflight = 0;
    std::vector<StorageLoad> m_storageLoads; // libraries to read, in request order
    std::uint64_t m_loadToken = 0;
    // The library was read as "nothing there" (a first run or a new account): until its index has
    // been created exclusively, storage that turns out to hold a library is re-read and merged,
    // never replaced.
    bool m_scopeFresh = false;
    bool m_lateScanOwed = false; // manifests may sit beside an index created without them
    bool m_loading = false;      // m_items is not yet the whole library of m_scope
    std::set<std::string> m_asyncPersistIds; // manifests the storage thread owes the disk
    std::set<std::string> m_rescanIds;       // items whose segment files must be re-read
    std::deque<StorageRemoval> m_storageRemovals;
    std::vector<StorageFinalWrite> m_storageFinalWrites; // a previous account's items, kept
    std::map<std::string, int> m_storageBusy;            // id -> removals queued or running
    std::map<std::string, bool> m_segmentsVerified;      // id -> last structural check result
    bool m_asyncIndexDue = false, m_persisterBusy = false, m_rescanAllDue = false;
    bool m_stop = false, m_playback = false, m_reconcileRequested = false,
         m_startupReconcile = false, m_configured = false;
    std::uint64_t m_generation = 0, m_nextPlanId = 1;
    std::set<std::string> m_deleteRequested;
    std::map<std::string, RecentSpeedSample> m_progressSamples;
    std::vector<DownloadItem> m_items;
    // Ids with a durable manifest on disk.  An id enters only after its
    // manifest write succeeded and leaves when the item is removed.  Every
    // index write is built from this set intersected with the live ids, so
    // the index never names an id whose manifest is absent.  Guarded by
    // m_mutex; every mutation and every index build share one acquisition.
    std::set<std::string> m_indexedIds;
    // Cached statvfs result so snapshot() (polled ~every 500ms) does not take
    // a syscall under the mutex on every call.  Guarded by m_mutex.
    mutable std::uint64_t m_freeBytesCached = 0, m_freeBytesCachedAtMs = 0;
    std::shared_ptr<library::LibraryQuery> m_libraryQuery;
    std::shared_ptr<library::LibraryCoordinator> m_libraryCoordinator;
    std::shared_ptr<std::atomic_bool> m_activePlanCancellation;
    std::shared_ptr<library::LibraryCoordinator> m_activePlanCoordinator;
    std::atomic<std::uint64_t> m_activePlanHierarchyRequest{0};
    struct PlanJob
    {
        std::uint64_t id, generation;
        Session session;
        std::vector<MediaItem> items;
        std::string seriesId, seasonId;
        MediaItem series, season;
        std::shared_ptr<library::LibraryQuery> libraryQuery;
        std::shared_ptr<library::LibraryCoordinator> libraryCoordinator;
        std::shared_ptr<std::atomic_bool> cancellation;
    };
    std::deque<PlanJob> m_planJobs;
    std::map<std::uint64_t, DownloadPlanSnapshot> m_plans;
};
}
#endif
