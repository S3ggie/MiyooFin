#include "CatalogDb.hpp"

#include "../cache/LibraryCache.hpp"
#include "../net/JellyfinApi.hpp"
#include "../../vendor/sqlite/sqlite3.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {

namespace {

constexpr unsigned char kDiagnosticsOperation = 1;
constexpr unsigned char kStatementReuseOperation = 2;
constexpr unsigned char kSqlErrorOperation = 3;
constexpr unsigned char kWriteSentinelOperation = 4;
constexpr unsigned char kReadSentinelOperation = 5;

bool validScopeIdentity(const std::string &serverUrl, const std::string &userId)
{
    const std::string normalizedUrl = JellyfinApi::normaliseUrl(serverUrl);
    const std::size_t schemeEnd = normalizedUrl.find("://");
    return schemeEnd != std::string::npos
        && schemeEnd + 3 < normalizedUrl.size()
        && userId.find_first_not_of(" \t\r\n") != std::string::npos;
}

bool makeDirectories(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string directory = path.substr(0, i);
            if (!directory.empty() && ::mkdir(directory.c_str(), 0755)
                && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

std::string catalogPath(const std::string &scopeKey)
{
    const std::string snapshot = LibraryCache::cachePath("cache", scopeKey);
    const std::size_t slash = snapshot.find_last_of('/');
    return snapshot.substr(0, slash + 1) + "catalog.sqlite3";
}

struct ScalarValue {
    std::string value;
};

int scalarCallback(void *context, int columnCount, char **values, char **)
{
    if (columnCount > 0 && values[0]) {
        static_cast<ScalarValue *>(context)->value = values[0];
    }
    return 0;
}

bool scalar(sqlite3 *db, const char *sql, std::string &value,
            std::string &error)
{
    ScalarValue result;
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, scalarCallback, &result, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    value = result.value;
    return true;
}

bool exec(sqlite3 *db, const char *sql, std::string &error)
{
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    return true;
}

} // namespace

struct CatalogDb::TestCommand {
    unsigned char operation;
    std::string value;
    std::promise<CatalogDbTestResult> result;
};

CatalogDb::CatalogDb()
    : m_worker(&CatalogDb::workerLoop, this)
{
}

CatalogDb::~CatalogDb()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        for (auto &queue : m_queues) {
            queue.clear();
        }
        m_scopeCommands.clear();
        m_testCommands.clear();
        m_pendingJobs = 0;
    }
    m_wake.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

CatalogDbEnqueueResult CatalogDb::enqueueNoopForTest(
    CatalogDbPriority priority,
    const CatalogDbJobMetadata &metadata)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            return CatalogDbEnqueueResult::RejectedStopping;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            return CatalogDbEnqueueResult::RejectedCancelled;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            return CatalogDbEnqueueResult::RejectedFull;
        }
        m_queues[priorityIndex(priority)].push_back({priority, metadata});
        ++m_pendingJobs;
    }
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

std::uint64_t CatalogDb::configureScope(const std::string &serverUrl,
                                        const std::string &userId)
{
    const bool validIdentity = validScopeIdentity(serverUrl, userId);
    const std::string scopeKey = validIdentity
        ? LibraryCache::scopeKey(serverUrl, userId) : std::string();
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey = scopeKey;
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = validIdentity ? CatalogDbScopeStatus::Pending
                                      : CatalogDbScopeStatus::InvalidIdentity;
        m_scopeCommands.clear();
        m_scopeCommands.push_back({
            validIdentity ? ScopeCommandKind::Configure
                          : ScopeCommandKind::InvalidIdentity,
            epoch, scopeKey});
    }
    m_wake.notify_one();
    return epoch;
}

std::uint64_t CatalogDb::deconfigureScope()
{
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey.clear();
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = CatalogDbScopeStatus::Unconfigured;
        m_scopeCommands.clear();
        m_scopeCommands.push_back({ScopeCommandKind::Deconfigure, epoch, {}});
    }
    m_wake.notify_one();
    return epoch;
}

CatalogDbScopeState CatalogDb::scopeState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_requestedEpoch, m_scopeConfigured, m_scopeReady, m_scopeStatus,
            m_lastError};
}

CatalogDbConnectionState CatalogDb::connectionStateForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_connectionOpen, m_connectionWorkerOwned,
            m_preparedStatementCount};
}

CatalogDbTestResult CatalogDb::runSqliteDiagnosticsForTest()
{
    return runTestCommand(kDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::runStatementReuseForTest()
{
    return runTestCommand(kStatementReuseOperation);
}

CatalogDbTestResult CatalogDb::runSqlErrorForTest()
{
    return runTestCommand(kSqlErrorOperation);
}

CatalogDbTestResult CatalogDb::writeSentinelForTest(const std::string &value)
{
    return runTestCommand(kWriteSentinelOperation, value);
}

CatalogDbTestResult CatalogDb::readSentinelForTest()
{
    return runTestCommand(kReadSentinelOperation);
}

CatalogDbEnqueueResult CatalogDb::enqueueScopedNoopForTest(
    CatalogDbPriority priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        return CatalogDbEnqueueResult::RejectedStopping;
    }
    if (!m_scopeConfigured || !m_scopeReady) {
        return CatalogDbEnqueueResult::RejectedScopeNotReady;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        return CatalogDbEnqueueResult::RejectedFull;
    }
    CatalogDbJobMetadata metadata;
    metadata.generation = m_generation;
    metadata.scopeEpoch = m_requestedEpoch;
    m_queues[priorityIndex(priority)].push_back({priority, metadata});
    ++m_pendingJobs;
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

bool CatalogDb::canPublishForTest(std::uint64_t scopeEpoch) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return scopeEpoch != 0 && scopeEpoch == m_requestedEpoch
        && m_scopeConfigured && m_scopeReady;
}

void CatalogDb::setGenerationForTest(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation = generation;
    }
    m_wake.notify_one();
}

void CatalogDb::setWorkerPausedForTest(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pausedForTest = paused;
    }
    m_wake.notify_one();
}

bool CatalogDb::waitForIdleForTest(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_idle.wait_for(lock, timeout, [this] {
        return m_pendingJobs == 0 && m_scopeCommands.empty() && !m_runningJob;
    });
}

std::vector<CatalogDbJobReport> CatalogDb::jobReportsForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_jobReports.begin(), m_jobReports.end()};
}

void CatalogDb::workerLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] {
            return m_stopping || (!m_pausedForTest
                && (!m_scopeCommands.empty() || !m_testCommands.empty()
                    || hasPendingJobsLocked()));
        });

        if (m_stopping) {
            lock.unlock();
            closeConnection();
            return;
        }

        if (!m_scopeCommands.empty()) {
            ScopeCommand command = std::move(m_scopeCommands.front());
            m_scopeCommands.pop_front();
            lock.unlock();
            processScopeCommand(std::move(command));
            continue;
        }

        if (!m_testCommands.empty()) {
            std::shared_ptr<TestCommand> command = std::move(m_testCommands.front());
            m_testCommands.pop_front();
            m_runningJob = true;
            lock.unlock();
            processTestCommand(command);
            lock.lock();
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        Job job = takeNextJobLocked();
        m_runningJob = true;
        lock.unlock();

        CatalogDbJobDisposition disposition = CatalogDbJobDisposition::Completed;
        if (job.metadata.cancellation && job.metadata.cancellation->load()) {
            disposition = CatalogDbJobDisposition::Cancelled;
        } else {
            std::lock_guard<std::mutex> stateLock(m_mutex);
            if (job.metadata.generation != m_generation
                || (job.metadata.scopeEpoch != 0
                    && (job.metadata.scopeEpoch != m_requestedEpoch
                        || !m_scopeConfigured || !m_scopeReady))) {
                disposition = CatalogDbJobDisposition::Superseded;
            }
        }

        lock.lock();
        if (m_jobReports.size() == kMaxPendingJobs) {
            m_jobReports.pop_front();
        }
        m_jobReports.push_back({job.priority, job.metadata, disposition});
        m_runningJob = false;
        if (m_pendingJobs == 0) {
            m_idle.notify_all();
        }
    }
}

bool CatalogDb::hasPendingJobsLocked() const
{
    return m_pendingJobs != 0;
}

CatalogDb::Job CatalogDb::takeNextJobLocked()
{
    for (std::size_t index = 0; index < m_queues.size(); ++index) {
        if (!m_queues[index].empty()) {
            Job job = m_queues[index].front();
            m_queues[index].pop_front();
            --m_pendingJobs;
            return job;
        }
    }

    return {CatalogDbPriority::Maintenance, {}};
}

std::size_t CatalogDb::priorityIndex(CatalogDbPriority priority)
{
    return static_cast<std::size_t>(priority);
}

void CatalogDb::processScopeCommand(ScopeCommand command)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch) {
            m_idle.notify_all();
            return;
        }

        m_scopeReady = false;
        m_scopeConfigured = false;
        m_scopeStatus = command.kind == ScopeCommandKind::InvalidIdentity
            ? CatalogDbScopeStatus::InvalidIdentity
            : CatalogDbScopeStatus::Pending;
        m_lastError = command.kind == ScopeCommandKind::InvalidIdentity
            ? CatalogDbErrorCategory::InvalidIdentity
            : CatalogDbErrorCategory::None;
    }

    closeConnection();

    if (command.kind == ScopeCommandKind::Deconfigure
        || command.kind == ScopeCommandKind::InvalidIdentity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_activeScopeKey.clear();
            m_scopeConfigured = false;
            m_scopeReady = false;
            m_scopeStatus = command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbScopeStatus::InvalidIdentity
                : CatalogDbScopeStatus::Unconfigured;
            m_lastError = command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbErrorCategory::InvalidIdentity
                : CatalogDbErrorCategory::None;
        }
        m_idle.notify_all();
        return;
    }

    openConnection(command);
    m_idle.notify_all();
}

void CatalogDb::closeConnection()
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    if (!m_db) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectionOpen = false;
        m_connectionWorkerOwned = false;
        m_preparedStatementCount = 0;
        return;
    }

    for (auto &entry : m_statements) {
        sqlite3_finalize(entry.second);
    }
    m_statements.clear();
    sqlite3_close(m_db);
    m_db = nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connectionOpen = false;
    m_connectionWorkerOwned = false;
    m_preparedStatementCount = 0;
}

bool CatalogDb::openConnection(const ScopeCommand &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            return false;
        }
    }

    const std::string path = catalogPath(command.scopeKey);
    const std::size_t slash = path.find_last_of('/');
    std::string error;
    if (slash == std::string::npos || !makeDirectories(path.substr(0, slash))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::OpenFailed;
        }
        return false;
    }

    sqlite3 *db = nullptr;
    const int openRc = sqlite3_open_v2(
        path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (openRc != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::OpenFailed;
        }
        return false;
    }

    sqlite3_extended_result_codes(db, 1);
    if (!exec(db, "PRAGMA foreign_keys = ON;", error)
        || !exec(db, "PRAGMA trusted_schema = OFF;", error)
        || !exec(db, "PRAGMA journal_mode = DELETE;", error)
        || !exec(db, "PRAGMA synchronous = FULL;", error)
        || !exec(db, "PRAGMA locking_mode = NORMAL;", error)) {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::ConfigurationFailed;
        }
        return false;
    }

    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    if (!scalar(db, "PRAGMA foreign_keys;", foreignKeys, error)
        || !scalar(db, "PRAGMA trusted_schema;", trustedSchema, error)
        || !scalar(db, "PRAGMA journal_mode;", journalMode, error)
        || !scalar(db, "PRAGMA synchronous;", synchronous, error)
        || !scalar(db, "PRAGMA locking_mode;", lockingMode, error)
        || foreignKeys != "1" || trustedSchema != "0"
        || journalMode != "delete" || synchronous != "2"
        || lockingMode != "normal") {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::ConfigurationFailed;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            sqlite3_close(db);
            return false;
        }
        m_db = db;
        m_connectionOpen = true;
        m_connectionWorkerOwned = std::this_thread::get_id() == m_worker.get_id();
        m_activeScopeKey = command.scopeKey;
        m_scopeConfigured = true;
        m_scopeReady = true;
        m_scopeStatus = CatalogDbScopeStatus::Ready;
        m_lastError = CatalogDbErrorCategory::None;
    }
    return true;
}

void CatalogDb::processTestCommand(const std::shared_ptr<TestCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbTestResult result;
    result.workerOwned = std::this_thread::get_id() == m_worker.get_id();
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        command->result.set_value(std::move(result));
        return;
    }

    auto prepareNamed = [&](const char *name, const char *sql,
                            sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        const int rc = sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };

    switch (command->operation) {
    case kDiagnosticsOperation:
        if (!scalar(m_db, "PRAGMA foreign_keys;", result.foreignKeys,
                    result.message)
            || !scalar(m_db, "PRAGMA trusted_schema;", result.trustedSchema,
                       result.message)
            || !scalar(m_db, "PRAGMA journal_mode;", result.journalMode,
                       result.message)
            || !scalar(m_db, "PRAGMA synchronous;", result.synchronous,
                       result.message)
            || !scalar(m_db, "PRAGMA locking_mode;", result.lockingMode,
                       result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        result.success = result.foreignKeys == "1"
            && result.trustedSchema == "0"
            && result.journalMode == "delete"
            && result.synchronous == "2"
            && result.lockingMode == "normal";
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "connection pragmas do not match the baseline";
        }
        break;

    case kStatementReuseOperation: {
        sqlite3_stmt *first = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", first)) {
            break;
        }
        int rc = sqlite3_step(first);
        reset(first);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        sqlite3_stmt *second = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", second)) {
            break;
        }
        reset(second);
        result.statementReused = first == second;
        result.success = result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "statement registry did not reuse the statement";
        }
        break;
    }

    case kSqlErrorOperation: {
        sqlite3_stmt *statement = nullptr;
        const int rc = sqlite3_prepare_v2(
            m_db, "SELECT * FROM catalog_task05_missing_table", -1,
            &statement, nullptr);
        int stepRc = rc;
        if (rc == SQLITE_OK) {
            stepRc = sqlite3_step(statement);
        }
        if (statement) {
            sqlite3_finalize(statement);
        }
        const bool sqliteFailed = stepRc != SQLITE_OK && stepRc != SQLITE_ROW
            && stepRc != SQLITE_DONE;
        result.success = false;
        result.error = sqliteFailed ? CatalogDbErrorCategory::SqliteError
                                    : CatalogDbErrorCategory::ConfigurationFailed;
        result.message = sqliteFailed ? sqlite3_errmsg(m_db)
                                      : "expected SQLite error was not returned";
        break;
    }

    case kWriteSentinelOperation: {
        if (!exec(m_db,
                  "CREATE TABLE IF NOT EXISTS catalog_task05_sentinel(value TEXT NOT NULL)",
                  result.message)
            || !exec(m_db, "DELETE FROM catalog_task05_sentinel",
                     result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_insert",
                          "INSERT INTO catalog_task05_sentinel(value) VALUES(?1)",
                          statement)) {
            break;
        }
        sqlite3_bind_text(statement, 1, command->value.c_str(), -1,
                          SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement);
        reset(statement);
        result.success = rc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kReadSentinelOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_read",
                          "SELECT value FROM catalog_task05_sentinel LIMIT 1",
                          statement)) {
            break;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(statement, 0);
            result.sentinel = value ? reinterpret_cast<const char *>(value) : "";
            result.success = true;
        } else {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        break;
    }

    default:
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "unknown CatalogDb test operation";
        break;
    }

    command->result.set_value(std::move(result));
}

CatalogDbTestResult CatalogDb::runTestCommand(unsigned char operation,
                                              const std::string &value)
{
    auto command = std::make_shared<TestCommand>();
    command->operation = operation;
    command->value = value;
    std::future<CatalogDbTestResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbTestResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            return stopped;
        }
        if (m_testCommands.size() >= kMaxPendingJobs) {
            CatalogDbTestResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb test command queue is full";
            return full;
        }
        m_testCommands.push_back(command);
    }
    m_wake.notify_one();
    return result.get();
}

} // namespace miyoofin
