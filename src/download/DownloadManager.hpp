#ifndef MIYOOFIN_DOWNLOAD_MANAGER_HPP
#define MIYOOFIN_DOWNLOAD_MANAGER_HPP
#include "DownloadStore.hpp"
#include "../net/Session.hpp"
#include "../data/MediaItem.hpp"
#include <condition_variable>
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
    std::uint64_t freeBytes() const;
    static bool statvfsFreeBytes(const DownloadStore& store, const std::string& scope,
                                 std::uint64_t& out);
    DownloadStore m_store;
    Session m_session;
    std::string m_scope;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake, m_planWake, m_reconcileWake;
    std::thread m_thread, m_planThread, m_reconcileThread;
    bool m_stop = false, m_playback = false, m_reconcileRequested = false,
         m_persistRequested = false, m_startupReconcile = false;
    std::uint64_t m_generation = 0, m_nextPlanId = 1;
    std::set<std::string> m_deleteRequested;
    std::map<std::string, RecentSpeedSample> m_progressSamples;
    std::vector<DownloadItem> m_items;
    // Ids enqueue() added since the worker's last persist pass.  The worker
    // persists exactly these manifests (read live, under the same lock
    // acquisition) plus the index, so a persist pass can never overwrite a
    // newer per-item state with stale bytes.  Guarded by m_mutex.
    std::set<std::string> m_persistPendingIds;
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
