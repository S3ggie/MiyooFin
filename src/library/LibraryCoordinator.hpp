#ifndef MIYOOFIN_LIBRARY_COORDINATOR_HPP
#define MIYOOFIN_LIBRARY_COORDINATOR_HPP

#include "LibraryQuery.hpp"
#include "LibrarySync.hpp"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace miyoofin {
namespace library {

/// Session-owned boundary for the shared online library services.
///
enum class StartupSyncMode { SkipFresh, DeltaCatchUp, FullReconcile };

struct StartupSyncResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    StartupSyncMode mode = StartupSyncMode::FullReconcile;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

/// The coordinator owns the lifecycle of LibrarySync and LibraryQuery and the
/// top-level startup sync policy.  LibrarySync remains the lower-level
/// CatalogDb/network primitive; this class is the one startup sync driver.
class LibraryCoordinator {
public:
    LibraryCoordinator(Session session, std::shared_ptr<CatalogDb> db,
                       std::uint64_t scopeEpoch);
    ~LibraryCoordinator();

    LibraryCoordinator(const LibraryCoordinator&) = delete;
    LibraryCoordinator& operator=(const LibraryCoordinator&) = delete;

    /// Start session-scoped live events. Safe to call more than once.
    void start();

    /// Start the coordinator-owned persisted-checkpoint decision and bounded
    /// startup catch-up. Full population remains HomeScreen-owned for now.
    bool startStartupSync(bool catalogHasRows);
    bool takeStartupSyncResult(StartupSyncResult &result);
    /// Cancel and join the startup operation without stopping session scope.
    void cancelStartupSync() noexcept;

    /// Cancel and join coordinator-owned work. Safe to call more than once.
    void stop() noexcept;

    bool running() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_running;
    }
    bool stopped() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_stopped;
    }

    std::shared_ptr<LibrarySync> sync() const { return m_sync; }
    std::shared_ptr<LibraryQuery> query() const { return m_query; }
    struct Status {
        bool inFlight = false;
        bool startupInFlight = false;
        bool cancelRequested = false;
        bool success = false;
        std::uint64_t generation = 0;
    };
    Status status() const;

private:
    std::shared_ptr<LibrarySync> m_sync;
    std::shared_ptr<LibraryQuery> m_query;
    std::shared_ptr<CatalogDb> m_db;
    std::uint64_t m_scopeEpoch = 0;
    bool m_running = false;
    bool m_stopped = false;

    mutable std::mutex m_startupMutex;
    std::condition_variable m_startupWake;
    std::thread m_startupThread;
    std::shared_ptr<std::atomic_bool> m_startupCancellation;
    StartupSyncResult m_startupResult;
    bool m_startupResultReady = false;
    bool m_startupInFlight = false;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_COORDINATOR_HPP
