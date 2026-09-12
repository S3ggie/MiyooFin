#ifndef MIYOOFIN_LIBRARY_SYNC_HPP
#define MIYOOFIN_LIBRARY_SYNC_HPP

#include "../catalog/CatalogDb.hpp"
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

struct HierarchyRefreshResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::vector<MediaItem> items;
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
    std::shared_ptr<std::atomic_bool> cancellation() const { return m_cancel; }
    std::future<CatalogDbTopLevelSyncResult> begin(std::uint64_t generation);
    std::future<CatalogDbMediaPageUpsertResult> stage(
        const CatalogDbMediaPageWrite &page);
    std::future<CatalogDbTopLevelSyncResult> finalize(std::uint64_t generation);
    std::future<CatalogDbTopLevelSyncResult> abort(std::uint64_t generation);
    std::future<HierarchyRefreshResult> refreshSeasons(
        const MediaItem &series,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    std::future<HierarchyRefreshResult> refreshEpisodes(
        const MediaItem &series, const MediaItem &season,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
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
};

}
}
#endif
