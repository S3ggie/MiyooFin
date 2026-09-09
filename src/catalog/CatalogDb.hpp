#ifndef MIYOOFIN_CATALOG_DB_HPP
#define MIYOOFIN_CATALOG_DB_HPP

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>

namespace miyoofin {

/// App-scoped owner for CatalogDb work. Database behavior is added by later
/// migration tasks; this shell only proves the worker lifecycle.
class CatalogDb {
public:
    CatalogDb();
    ~CatalogDb();

    CatalogDb(const CatalogDb&) = delete;
    CatalogDb& operator=(const CatalogDb&) = delete;

    /// Queue a lifecycle-only job for focused host tests.
    void enqueueNoopForTest();

private:
    void workerLoop();

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<unsigned char> m_noopJobs;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace miyoofin

#endif // MIYOOFIN_CATALOG_DB_HPP
