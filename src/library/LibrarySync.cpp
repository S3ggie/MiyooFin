#include "LibrarySync.hpp"

namespace miyoofin {
namespace library {
library::LibrarySync::LibrarySync(Session session, std::shared_ptr<CatalogDb> db,
                         std::uint64_t scopeEpoch)
    : m_session(std::move(session)), m_db(std::move(db)),
      m_cancel(std::make_shared<std::atomic_bool>(false)) {
    m_metadata.scopeEpoch = scopeEpoch;
    m_metadata.cancellation = m_cancel;
}
}
library::LibrarySync::~LibrarySync() { cancel(); }
std::uint64_t library::LibrarySync::nextGeneration() { return ++m_generation; }
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::begin(std::uint64_t generation) {
    m_inFlight = true; m_success = false;
    return m_db->beginTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbMediaPageUpsertResult> library::LibrarySync::stage(const CatalogDbMediaPageWrite &page) {
    return m_db->upsertMediaPage(page, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::finalize(std::uint64_t generation) {
    m_success = true; m_inFlight = false;
    return m_db->finalizeTopLevelSync(generation, m_metadata);
}
std::future<CatalogDbTopLevelSyncResult> library::LibrarySync::abort(std::uint64_t generation) {
    m_inFlight = false;
    return m_db->abortTopLevelSync(generation, m_metadata);
}
library::LibrarySync::Status library::LibrarySync::status() const {
    return {m_inFlight.load(), m_generation.load(), m_success.load()};
}
void library::LibrarySync::cancel() noexcept { if (m_cancel) m_cancel->store(true); }
}
