#ifndef MIYOOFIN_LIBRARY_SYNC_HPP
#define MIYOOFIN_LIBRARY_SYNC_HPP

#include "../catalog/CatalogDb.hpp"
#include "../net/Session.hpp"
#include <atomic>
#include <cstdint>
#include <future>
#include <memory>

namespace miyoofin {
namespace library {

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
    Status status() const;
    void cancel() noexcept;

private:
    Session m_session;
    std::shared_ptr<CatalogDb> m_db;
    CatalogDbJobMetadata m_metadata;
    std::shared_ptr<std::atomic_bool> m_cancel;
    std::atomic<std::uint64_t> m_generation{0};
    std::atomic<bool> m_inFlight{false};
    std::atomic<bool> m_success{false};
};

}
}
#endif
