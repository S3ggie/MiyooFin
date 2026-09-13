#include "LibrarySync.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include <thread>

namespace miyoofin {
namespace library {

void library::LibrarySync::startLiveEvents()
{
    std::lock_guard<std::mutex> lock(m_liveEventMutex);
    if (m_liveEventThread.joinable() || !m_session.valid()
        || m_session.manualOfflineMode)
        return;
    m_liveEventQueue = std::make_shared<JellyfinLibraryEventQueue>();
    m_liveEventCancellation = std::make_shared<std::atomic_bool>(false);
    const Session session = m_session;
    const auto queue = m_liveEventQueue;
    const auto cancellation = m_liveEventCancellation;
    m_liveEventThread = std::thread([session, queue, cancellation] {
        JellyfinLibraryEvents events(session);
        (void)events.run(*queue, cancellation);
    });
}

bool library::LibrarySync::takeLiveChange(JellyfinLibraryChangeBatch &batch)
{
    std::shared_ptr<JellyfinLibraryEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(m_liveEventMutex);
        queue = m_liveEventQueue;
    }
    return queue && queue->pop(batch);
}

library::LibrarySync::Status library::LibrarySync::status() const {
    return {m_inFlight.load(), m_generation.load(), m_success.load()};
}
}
} // namespace miyoofin
