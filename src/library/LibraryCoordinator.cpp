#include "LibraryCoordinator.hpp"

namespace miyoofin {
namespace library {

LibraryCoordinator::LibraryCoordinator(Session session,
                                       std::shared_ptr<CatalogDb> db,
                                       std::uint64_t scopeEpoch)
    : m_sync(std::make_shared<LibrarySync>(std::move(session), db,
                                            scopeEpoch))
    , m_query(std::make_shared<LibraryQuery>(db, scopeEpoch))
{
}

LibraryCoordinator::~LibraryCoordinator()
{
    stop();
}

void LibraryCoordinator::start()
{
    if (m_stopped || m_running || !m_sync)
        return;
    m_sync->startLiveEvents();
    m_running = true;
}

void LibraryCoordinator::stop() noexcept
{
    if (m_stopped)
        return;
    if (m_sync)
        m_sync->stop();
    m_running = false;
    m_stopped = true;
}

LibrarySync::Status LibraryCoordinator::status() const
{
    return m_sync ? m_sync->status() : LibrarySync::Status{};
}

} // namespace library
} // namespace miyoofin
