#ifndef MIYOOFIN_LIBRARY_SYNC_HPP
#define MIYOOFIN_LIBRARY_SYNC_HPP

#include "../catalog/CatalogDb.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/Session.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace miyoofin {
namespace library {

struct OfflineRebuildResult {
    bool success = false;
    bool skipped = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::size_t itemsUpserted = 0;
    std::size_t containersSynthesized = 0;
};

struct ChangedCatalogResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::size_t itemsUpserted = 0;
    std::int64_t checkpointMs = 0;
};

struct MembershipReconcileResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::size_t pagesRead = 0;
    std::size_t itemsStaged = 0;
};

struct LiveLibraryChangeResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    bool catchUpRequired = false;
    bool userDataChanged = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::size_t itemsFetched = 0;
    std::size_t itemsUpserted = 0;
    std::size_t itemsRemoved = 0;
    std::vector<MediaItem> items;
    std::vector<std::string> removedIds;
};

// App-scoped owner for top-level Jellyfin -> CatalogDb generation work.  The
// CatalogDb remains the sole SQLite executor; this class owns only the
// synchronization boundary and its cancellation/status state.
class LibrarySync {
public:
    struct Status { bool inFlight = false; std::uint64_t generation = 0; bool success = false; };

    LibrarySync(Session session, std::shared_ptr<CatalogDb> db,
                std::uint64_t scopeEpoch);
    ~LibrarySync();
    LibrarySync(const LibrarySync&) = delete;
    LibrarySync& operator=(const LibrarySync&) = delete;

    std::uint64_t nextGeneration();
    /// Seed the generation counter so session writes start at or above the
    /// persisted committed_generation.  Only has an effect when `gen` exceeds
    /// the current counter.
    void seedGeneration(std::uint64_t gen);
    std::shared_ptr<std::atomic_bool> cancellation() const { return m_cancel; }
    std::future<CatalogDbTopLevelSyncResult> begin(std::uint64_t generation);
    std::future<CatalogDbMediaPageUpsertResult> stage(
        const CatalogDbMediaPageWrite &page);
    std::future<CatalogDbTopLevelSyncResult> finalize(std::uint64_t generation);
    std::future<CatalogDbTopLevelSyncResult> abort(std::uint64_t generation);
    std::future<ChangedCatalogResult> catchUpChangedCatalog(
        std::int64_t sinceMs,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<MembershipReconcileResult> reconcileAuthoritativeMembership(
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<LiveLibraryChangeResult> applyLibraryChanges(
        const JellyfinLibraryChangeBatch &batch,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    /// Start the long-lived Jellyfin event receiver. The receiver only owns
    /// its bounded queue; all catalog writes remain in LibrarySync methods.
    void startLiveEvents();
    bool takeLiveChange(JellyfinLibraryChangeBatch &batch);
    std::future<CatalogDbReconcileResult> reconcileSeries(
        const std::vector<MediaItem> &series, bool authoritative,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<CatalogDbHierarchyWriteResult> stageSeriesHierarchy(
        const MediaItem &series, const std::vector<MediaItem> &seasons,
        const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
        std::uint64_t generation, bool complete,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<CatalogDbSyncState> writeSyncState(
        std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
        std::uint64_t committedGeneration,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<OfflineRebuildResult> reconstructOfflineDownloads(
        const std::string &downloadRoot = "downloads");
    Status status() const;
    /// Cancel service work and join the long-lived live-event receiver.
    /// Operation futures owned by consumers observe the same cancellation
    /// token and remain the responsibility of those consumers to join.
    void stop() noexcept;
    void cancel() noexcept;

private:
    Session m_session;
    std::shared_ptr<CatalogDb> m_db;
    CatalogDbJobMetadata m_metadata;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::atomic<std::uint64_t> m_generation{0};
    std::shared_ptr<std::atomic<std::uint64_t>> m_offlineGeneration;
    std::shared_ptr<std::atomic_bool> m_offlineCancellation;
    mutable std::mutex m_offlineMutex;
    std::atomic<bool> m_inFlight{false};
    std::atomic<bool> m_success{false};
    std::atomic<bool> m_authoritativeSyncInFlight{false};
    std::shared_ptr<JellyfinLibraryEventQueue> m_liveEventQueue;
    std::shared_ptr<std::atomic_bool> m_liveEventCancellation;
    std::thread m_liveEventThread;
    mutable std::mutex m_liveEventMutex;
};

}
}
#endif
