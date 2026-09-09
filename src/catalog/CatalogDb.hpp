#ifndef MIYOOFIN_CATALOG_DB_HPP
#define MIYOOFIN_CATALOG_DB_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

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
    RejectedScopeNotReady,
};

enum class CatalogDbJobDisposition : unsigned char {
    Completed,
    Cancelled,
    Superseded,
};

enum class CatalogDbScopeStatus : unsigned char {
    Unconfigured,
    Pending,
    Ready,
    InvalidIdentity,
    OpenFailed,
};

enum class CatalogDbErrorCategory : unsigned char {
    None,
    InvalidIdentity,
    ScopeNotReady,
    OpenFailed,
    ConfigurationFailed,
    SqliteError,
    Superseded,
};

struct CatalogDbScopeState {
    std::uint64_t requestedEpoch = 0;
    bool configured = false;
    bool ready = false;
    CatalogDbScopeStatus status = CatalogDbScopeStatus::Unconfigured;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
};

struct CatalogDbJobMetadata {
    std::uint64_t generation = 0;
    std::uint64_t scopeEpoch = 0;
    std::shared_ptr<std::atomic_bool> cancellation;
};

struct CatalogDbJobReport {
    CatalogDbPriority priority;
    CatalogDbJobMetadata metadata;
    CatalogDbJobDisposition disposition;
};

struct CatalogDbConnectionState {
    bool open = false;
    bool workerOwned = false;
    std::size_t preparedStatements = 0;
};

struct CatalogDbTestResult {
    bool success = false;
    bool workerOwned = false;
    bool statementReused = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    std::string sentinel;
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

    /// Request a scope transition without doing the transition on the caller
    /// thread. Every request deliberately refreshes the epoch, including an
    /// exact repeat of the current server/user identity.
    std::uint64_t configureScope(const std::string &serverUrl,
                                 const std::string &userId);
    std::uint64_t deconfigureScope();
    CatalogDbScopeState scopeState() const;

    /// Queue a scope-bound lifecycle job using the currently requested epoch.
    CatalogDbEnqueueResult enqueueScopedNoopForTest(
        CatalogDbPriority priority);
    bool canPublishForTest(std::uint64_t scopeEpoch) const;
    CatalogDbConnectionState connectionStateForTest() const;
    CatalogDbTestResult runSqliteDiagnosticsForTest();
    CatalogDbTestResult runStatementReuseForTest();
    CatalogDbTestResult runSqlErrorForTest();
    CatalogDbTestResult writeSentinelForTest(const std::string &value);
    CatalogDbTestResult readSentinelForTest();

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

    enum class ScopeCommandKind : unsigned char {
        Configure,
        Deconfigure,
        InvalidIdentity,
    };

    struct ScopeCommand {
        ScopeCommandKind kind;
        std::uint64_t epoch;
        std::string scopeKey;
    };

    struct TestCommand;

    void workerLoop();
    bool hasPendingJobsLocked() const;
    Job takeNextJobLocked();
    static std::size_t priorityIndex(CatalogDbPriority priority);
    void processScopeCommand(ScopeCommand command);
    void processTestCommand(const std::shared_ptr<TestCommand> &command);
    void closeConnection();
    bool openConnection(const ScopeCommand &command);
    CatalogDbTestResult runTestCommand(unsigned char operation,
                                       const std::string &value = {});

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::array<std::deque<Job>, 4> m_queues;
    std::deque<ScopeCommand> m_scopeCommands;
    std::deque<std::shared_ptr<TestCommand>> m_testCommands;
    std::deque<CatalogDbJobReport> m_jobReports;
    std::uint64_t m_generation = 0;
    std::uint64_t m_requestedEpoch = 0;
    std::string m_requestedScopeKey;
    std::string m_activeScopeKey;
    std::size_t m_pendingJobs = 0;
    bool m_runningJob = false;
    bool m_pausedForTest = false;
    bool m_scopeConfigured = false;
    bool m_scopeReady = false;
    CatalogDbScopeStatus m_scopeStatus = CatalogDbScopeStatus::Unconfigured;
    CatalogDbErrorCategory m_lastError = CatalogDbErrorCategory::None;
    bool m_stopping = false;
    std::thread m_worker;
    sqlite3 *m_db = nullptr;
    std::map<std::string, sqlite3_stmt *> m_statements;
    bool m_connectionOpen = false;
    bool m_connectionWorkerOwned = false;
    std::size_t m_preparedStatementCount = 0;
};

} // namespace miyoofin

#endif // MIYOOFIN_CATALOG_DB_HPP
