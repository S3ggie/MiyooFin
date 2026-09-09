#include "CatalogDb.hpp"

namespace miyoofin {

CatalogDb::CatalogDb()
    : m_worker(&CatalogDb::workerLoop, this)
{
}

CatalogDb::~CatalogDb()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        m_noopJobs.clear();
    }
    m_wake.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

void CatalogDb::enqueueNoopForTest()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return;
        }
        m_noopJobs.push_back(0);
    }
    m_wake.notify_one();
}

void CatalogDb::workerLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] {
            return m_stopping || !m_noopJobs.empty();
        });

        if (m_stopping) {
            return;
        }

        m_noopJobs.pop_front();
    }
}

} // namespace miyoofin
