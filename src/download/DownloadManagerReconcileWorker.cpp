#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "DownloadReconcile.hpp"

namespace miyoofin {

void DownloadManager::requestReconcile(){{std::lock_guard<std::mutex>l(m_mutex);if(m_session.valid())m_reconcileRequested=true;}m_reconcileWake.notify_one();}

} // namespace miyoofin
