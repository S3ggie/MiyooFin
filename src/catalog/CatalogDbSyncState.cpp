#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

namespace miyoofin {
using namespace catalog_db_internal;

std::future<CatalogDbSyncState> CatalogDb::readSyncState(
    bool legacyAvailable, std::int64_t legacyLastSuccessfulMs,
    std::int64_t legacyLastReconcileMs, const CatalogDbJobMetadata &metadata)
{
    return enqueueSyncStateRead(legacyAvailable, legacyLastSuccessfulMs,
                                legacyLastReconcileMs, metadata);
}
std::future<CatalogDbSyncState> CatalogDb::writeSyncState(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
    std::uint64_t committedGeneration, const CatalogDbJobMetadata &metadata)
{
    return enqueueSyncStateWrite(lastSuccessfulMs, lastReconcileMs,
                                 committedGeneration, metadata);
}
std::future<CatalogDbSyncState> CatalogDb::enqueueSyncStateRead(
    bool legacyAvailable, std::int64_t legacyLastSuccessfulMs,
    std::int64_t legacyLastReconcileMs, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<SyncStateCommand>();
    command->legacyAvailable = legacyAvailable;
    command->legacyLastSuccessfulMs = legacyLastSuccessfulMs;
    command->legacyLastReconcileMs = legacyLastReconcileMs;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    std::future<CatalogDbSyncState> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbSyncState stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            CatalogDbSyncState cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb sync-state read cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            CatalogDbSyncState full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb sync-state queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0)
            command->metadata.generation = m_generation;
        if (command->metadata.scopeEpoch == 0)
            command->metadata.scopeEpoch = m_requestedEpoch;
        m_syncStateCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
std::future<CatalogDbSyncState> CatalogDb::enqueueSyncStateWrite(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
    std::uint64_t committedGeneration, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<SyncStateCommand>();
    command->write = true;
    command->lastSuccessfulMs = lastSuccessfulMs;
    command->lastReconcileMs = lastReconcileMs;
    command->committedGeneration = committedGeneration;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    std::future<CatalogDbSyncState> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbSyncState stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            CatalogDbSyncState cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb sync-state write cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            CatalogDbSyncState full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb sync-state queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0)
            command->metadata.generation = m_generation;
        if (command->metadata.scopeEpoch == 0)
            command->metadata.scopeEpoch = m_requestedEpoch;
        m_syncStateCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}
void CatalogDb::processSyncState(
    const std::shared_ptr<SyncStateCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbSyncState result;
    result.workerOwned = true;
    auto finish = [&] {
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0
            && endUs >= command->enqueuedMonotonicUs) {
            performanceTelemetry().recordCatalogDbQueueWait(
                endUs - command->enqueuedMonotonicUs);
        }
        if (result.cancelled || result.superseded)
            performanceTelemetry().addCatalogDbCancelled();
        else if (result.success)
            performanceTelemetry().addCatalogDbCompleted();
        else
            performanceTelemetry().addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    auto stateIsValid = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb sync-state operation cancelled";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb sync-state operation superseded";
            return false;
        }
        return true;
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!stateIsValid()) {
        finish();
        return;
    }

    if (command->write) {
        if (command->lastSuccessfulMs < 0 || command->lastReconcileMs < 0) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "sync-state timestamps must be non-negative";
            finish();
            return;
        }
        sqlite3_stmt *current = nullptr;
        const bool currentPrepared = sqlite3_prepare_v2(
            m_db,
            "SELECT last_successful_ms, last_reconcile_ms, "
            "committed_generation FROM sync_state WHERE singleton_id=1",
            -1, &current, nullptr) == SQLITE_OK;
        if (!currentPrepared || sqlite3_step(current) != SQLITE_ROW) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            if (current)
                sqlite3_finalize(current);
            finish();
            return;
        }
        const auto currentSuccessful = sqlite3_column_int64(current, 0);
        const auto currentReconcile = sqlite3_column_int64(current, 1);
        const auto currentGenerationValue = sqlite3_column_int64(current, 2);
        const auto currentGeneration = static_cast<std::uint64_t>(
            currentGenerationValue);
        sqlite3_finalize(current);
        const bool currentMalformed = currentSuccessful < 0
            || currentReconcile < 0
            || currentGenerationValue < 0
            || (currentSuccessful == 0 && currentReconcile != 0)
            || (currentSuccessful > 0 && currentReconcile > currentSuccessful);
        const bool candidateMalformed = command->lastSuccessfulMs == 0
            ? command->lastReconcileMs != 0
            : command->lastReconcileMs > command->lastSuccessfulMs;
        const bool regressed = command->lastSuccessfulMs < currentSuccessful
            || command->lastReconcileMs < currentReconcile
            || command->committedGeneration < currentGeneration;
        if (currentMalformed || candidateMalformed || regressed) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = currentMalformed
                ? "stored sync-state checkpoint is invalid; full reconcile required"
                : "sync-state checkpoint regressed; full reconcile required";
            finish();
            return;
        }
        if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr)
                != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        }
        sqlite3_stmt *statement = nullptr;
        const bool prepared = sqlite3_prepare_v2(
            m_db,
            "UPDATE sync_state SET last_successful_ms=?1, "
            "last_reconcile_ms=?2, committed_generation=?3 "
            "WHERE singleton_id=1",
            -1, &statement, nullptr) == SQLITE_OK;
        const bool updated = prepared
            && sqlite3_bind_int64(statement, 1, command->lastSuccessfulMs)
                   == SQLITE_OK
            && sqlite3_bind_int64(statement, 2, command->lastReconcileMs)
                   == SQLITE_OK
            && sqlite3_bind_int64(
                   statement, 3,
                   static_cast<sqlite3_int64>(command->committedGeneration))
                   == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_DONE;
        if (statement)
            sqlite3_finalize(statement);
        if (!updated || !stateIsValid()
            || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr)
                   != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            result.error = result.error == CatalogDbErrorCategory::None
                ? CatalogDbErrorCategory::SqliteError : result.error;
            if (result.message.empty())
                result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        }
        result.success = true;
        result.lastSuccessfulMs = command->lastSuccessfulMs;
        result.lastReconcileMs = command->lastReconcileMs;
        result.committedGeneration = command->committedGeneration;
        finish();
        return;
    }

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(
            m_db,
            "SELECT last_successful_ms, last_reconcile_ms, "
            "committed_generation FROM sync_state WHERE singleton_id=1",
            -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_step(statement) != SQLITE_ROW) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        if (statement)
            sqlite3_finalize(statement);
        finish();
        return;
    }
    result.lastSuccessfulMs = sqlite3_column_int64(statement, 0);
    result.lastReconcileMs = sqlite3_column_int64(statement, 1);
    result.committedGeneration = static_cast<std::uint64_t>(
        sqlite3_column_int64(statement, 2));
    sqlite3_finalize(statement);

    const bool legacyHasCheckpoint = command->legacyLastSuccessfulMs > 0
        || command->legacyLastReconcileMs > 0;
    if (command->legacyAvailable && legacyHasCheckpoint
        && result.lastSuccessfulMs == 0 && result.lastReconcileMs == 0
        && result.committedGeneration == 0) {
        if (command->legacyLastSuccessfulMs < 0
            || command->legacyLastReconcileMs < 0) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "legacy sync-state timestamps are invalid";
            finish();
            return;
        }
        if (sqlite3_exec(m_db, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr)
                != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        }
        sqlite3_stmt *seed = nullptr;
        const bool prepared = sqlite3_prepare_v2(
            m_db,
            "UPDATE sync_state SET last_successful_ms=?1, "
            "last_reconcile_ms=?2 WHERE singleton_id=1 AND "
            "last_successful_ms=0 AND last_reconcile_ms=0 AND "
            "committed_generation=0",
            -1, &seed, nullptr) == SQLITE_OK;
        const bool seeded = prepared
            && sqlite3_bind_int64(seed, 1, command->legacyLastSuccessfulMs)
                   == SQLITE_OK
            && sqlite3_bind_int64(seed, 2, command->legacyLastReconcileMs)
                   == SQLITE_OK
            && sqlite3_step(seed) == SQLITE_DONE;
        if (seed)
            sqlite3_finalize(seed);
        if (!seeded || !stateIsValid()
            || sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, nullptr)
                   != SQLITE_OK) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            result.error = result.error == CatalogDbErrorCategory::None
                ? CatalogDbErrorCategory::SqliteError : result.error;
            if (result.message.empty())
                result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        }
        result.lastSuccessfulMs = command->legacyLastSuccessfulMs;
        result.lastReconcileMs = command->legacyLastReconcileMs;
        result.migrated = true;
    }
    result.success = true;
    finish();
}
} // namespace miyoofin
