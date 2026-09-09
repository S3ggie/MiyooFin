#ifndef MIYOOFIN_CATALOG_DB_HPP
#define MIYOOFIN_CATALOG_DB_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace miyoofin {

enum class CatalogDbPriority : unsigned char {
    InteractiveRead,
    ForegroundMetadataWrite,
    BackgroundSync,
    Maintenance,
};

enum class CatalogDbEnqueueResult : unsigned char {
    Accepted,
    RejectedFull,
    RejectedStopping,
    RejectedCancelled,
};

enum class CatalogDbJobDisposition : unsigned char {
    Completed,
    Cancelled,
    Superseded,
};

struct CatalogDbJobMetadata {
    std::uint64_t generation = 0;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct CatalogDbJobReport {
    CatalogDbPriority priority;
    CatalogDbJobMetadata metadata;
    CatalogDbJobDisposition disposition;
};

/// App-scoped owner for CatalogDb work. Database behavior is added by later
/// migration tasks; this shell only proves the worker lifecycle.
class CatalogDb {
public:
    static constexpr std::size_t kMaxPendingJobs = 32;

    CatalogDb();
    ~CatalogDb();

    CatalogDb(const CatalogDb&) = delete;
    CatalogDb& operator=(const CatalogDb&) = delete;

    /// Queue a lifecycle-only job for focused host tests.
    CatalogDbEnqueueResult enqueueNoopForTest(
        CatalogDbPriority priority,
        const CatalogDbJobMetadata &metadata = {});

    /// Set the generation accepted by the worker. Later scope work will use
    /// the same mechanism to suppress stale queued results.
    void setGenerationForTest(std::uint64_t generation);

    /// Test-only worker controls make queue behavior deterministic without
    /// exposing the worker, mutex, or condition variable to callers.
    void setWorkerPausedForTest(bool paused);
    bool waitForIdleForTest(std::chrono::milliseconds timeout);
    std::vector<CatalogDbJobReport> jobReportsForTest() const;

private:
    struct Job {
        CatalogDbPriority priority;
        CatalogDbJobMetadata metadata;
    };

    void workerLoop();
    bool hasPendingJobsLocked() const;
    Job takeNextJobLocked();
    static std::size_t priorityIndex(CatalogDbPriority priority);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::array<std::deque<Job>, 4> m_queues;
    std::deque<CatalogDbJobReport> m_jobReports;
    std::uint64_t m_generation = 0;
    std::size_t m_pendingJobs = 0;
    bool m_runningJob = false;
    bool m_pausedForTest = false;
    bool m_stopping = false;
    std::thread m_worker;
};

} // namespace miyoofin

#endif // MIYOOFIN_CATALOG_DB_HPP
