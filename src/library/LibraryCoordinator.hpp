#ifndef MIYOOFIN_LIBRARY_COORDINATOR_HPP
#define MIYOOFIN_LIBRARY_COORDINATOR_HPP

#include "LibraryQuery.hpp"
#include "LibrarySync.hpp"
#include <cstdint>
#include <memory>

namespace miyoofin {
namespace library {

/// Session-owned boundary for the shared online library services.
///
/// The coordinator owns the lifecycle of LibrarySync and LibraryQuery, while
/// existing screens and download code temporarily receive the shared service
/// pointers through the accessors below.  It deliberately does not duplicate
/// synchronization policy: LibrarySync remains the only sync driver.
class LibraryCoordinator {
public:
    LibraryCoordinator(Session session, std::shared_ptr<CatalogDb> db,
                       std::uint64_t scopeEpoch);
    ~LibraryCoordinator();

    LibraryCoordinator(const LibraryCoordinator&) = delete;
    LibraryCoordinator& operator=(const LibraryCoordinator&) = delete;

    /// Start session-scoped live events. Safe to call more than once.
    void start();

    /// Cancel and join coordinator-owned work. Safe to call more than once.
    void stop() noexcept;

    bool running() const noexcept { return m_running; }
    bool stopped() const noexcept { return m_stopped; }

    std::shared_ptr<LibrarySync> sync() const { return m_sync; }
    std::shared_ptr<LibraryQuery> query() const { return m_query; }
    LibrarySync::Status status() const;

private:
    std::shared_ptr<LibrarySync> m_sync;
    std::shared_ptr<LibraryQuery> m_query;
    bool m_running = false;
    bool m_stopped = false;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_COORDINATOR_HPP
