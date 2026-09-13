#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

#include "CatalogCompatibility.hpp"
#include "../data/MediaItem.hpp"
#include "MediaItemSql.hpp"
#include "CatalogDbSchema.hpp"
#include "CatalogPrimitives.hpp"
#include "../app/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include "../../vendor/sqlite/sqlite3.h"

#include <cassert>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {
using namespace catalog_db_internal;


struct CatalogDb::TestCommand {
    unsigned char operation;
    std::string value;
    std::promise<CatalogDbTestResult> result;
};

;

;

struct CatalogDb::ReconcileCommand {
    std::vector<MediaItem> series;
    bool authoritative = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterRows = -1;
    std::promise<CatalogDbReconcileResult> result;
};

struct CatalogDb::SyncStateCommand {
    bool write = false;
    bool legacyAvailable = false;
    std::int64_t legacyLastSuccessfulMs = 0;
    std::int64_t legacyLastReconcileMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::uint64_t committedGeneration = 0;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbSyncState> result;
};

struct CatalogDb::LibrarySeedCommand {
    CatalogCompatibilitySeedRequest request;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterWrites = -1;
    std::promise<CatalogCompatibilitySeedResult> result;
};
struct CatalogDb::LibraryReadCommand {
    CatalogDbJobMetadata metadata;
    std::promise<CatalogCompatibilityReadResult> result;
};
struct CatalogDb::MediaPageCommand {
    std::string type; int letter = -1; std::size_t limit = 0;
    CatalogDbMediaPageFilter filter = CatalogDbMediaPageFilter::Supported;
    CatalogDbPageCursor after; CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbMediaPageResult> result;
};
;
;

CatalogDb::CatalogDb()
    : m_worker(&CatalogDb::workerLoop, this)
{
    catalogDiagnostic("service_started");
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
        m_queryCommands.clear();
        m_writeCommands.clear();
        m_reconcileCommands.clear();
        m_syncStateCommands.clear();
        m_librarySeedCommands.clear();
        m_libraryReadCommands.clear();
        m_mediaPageCommands.clear();
        for (auto &command : m_mediaPageUpsertCommands) {
            CatalogDbMediaPageUpsertResult result;
            result.error = CatalogDbErrorCategory::ScopeNotReady;
            result.message = "CatalogDb stopped before page submission";
            command->result.set_value(std::move(result));
        }
        m_mediaPageUpsertCommands.clear();
        for (auto &command : m_topLevelSyncCommands) {
            CatalogDbTopLevelSyncResult result;
            result.error = CatalogDbErrorCategory::ScopeNotReady;
            result.message = "CatalogDb stopped before sync staging";
            command->result.set_value(std::move(result));
        }
        m_topLevelSyncCommands.clear();
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
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedStopping;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedCancelled;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedFull;
        }
        m_queues[priorityIndex(priority)].push_back(
            {priority, metadata, telemetryNowIfEnabled()});
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

std::uint64_t CatalogDb::configureScope(const std::string &serverUrl,
                                        const std::string &userId)
{
    const bool validIdentity = validScopeIdentity(serverUrl, userId);
    const std::string scopeKey = validIdentity
        ? catalog::scopeKey(serverUrl, userId) : std::string();
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
        m_lastError = validIdentity ? CatalogDbErrorCategory::None
                                    : CatalogDbErrorCategory::InvalidIdentity;
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
        m_scopeCommands.clear();
        m_scopeCommands.push_back({
            validIdentity ? ScopeCommandKind::Configure
                          : ScopeCommandKind::InvalidIdentity,
            epoch, scopeKey});
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "configure_scope_requested epoch=%llu scope_hash=%s identity=%s",
                  static_cast<unsigned long long>(epoch),
                  scopeKey.empty() ? "none" : scopeKey.c_str(),
                  validIdentity ? "valid" : "invalid");
    catalogDiagnostic(line);
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
        m_lastError = CatalogDbErrorCategory::None;
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
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
            m_lastError, m_openState, m_migrationState};
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

CatalogDbTestResult CatalogDb::runSchemaDiagnosticsForTest()
{
    return runTestCommand(kSchemaDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::writeSchemaMarkerForTest(const std::string &value)
{
    return runTestCommand(kWriteSchemaMarkerOperation, value);
}

CatalogDbTestResult CatalogDb::readSchemaMarkerForTest()
{
    return runTestCommand(kReadSchemaMarkerOperation);
}

CatalogDbTestResult CatalogDb::setSchemaMetadataForTest(
    std::int64_t applicationId, std::int64_t userVersion)
{
    return runTestCommand(kSetSchemaMetadataOperation,
                          std::to_string(applicationId) + ":"
                              + std::to_string(userVersion));
}

CatalogDbTestResult CatalogDb::runMigrationRollbackForTest()
{
    return runTestCommand(kMigrationRollbackOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCodecForTest()
{
    return runTestCommand(kMediaItemCodecOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCollectionsForTest()
{
    return runTestCommand(kMediaItemCollectionsOperation);
}

CatalogDbTestResult CatalogDb::seedHierarchyQueryFixturesForTest()
{
    return runTestCommand(kSeedHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::clearHierarchyQueryFixturesForTest()
{
    return runTestCommand(kClearHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::runMediaPageQueryPlanForTest(
    const std::string &type, int alphabetLetter)
{
    if (type != "movie" && type != "show") {
        CatalogDbTestResult result;
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "invalid media page query-plan type";
        return result;
    }
    return runTestCommand(kMediaPageQueryPlanOperation,
                          type + ":" + std::to_string(alphabetLetter));
}

CatalogDbTestResult CatalogDb::writeSentinelForTest(const std::string &value)
{
    return runTestCommand(kWriteSentinelOperation, value);
}

CatalogDbTestResult CatalogDb::readSentinelForTest()
{
    return runTestCommand(kReadSentinelOperation);
}

std::future<CatalogDbHierarchyResult> CatalogDb::getSeasons(
    const std::string &seriesId, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Seasons;
    command->parentId = seriesId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbHierarchyResult> CatalogDb::getEpisodes(
    const std::string &seasonId, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Episodes;
    command->parentId = seasonId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbHierarchyResult> CatalogDb::readMediaItemsByIds(
    const std::vector<std::string> &itemIds,
    const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::MediaItemsByIds;
    command->itemIds = itemIds;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        CatalogDbHierarchyResult rejected;
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::ScopeNotReady;
            rejected.message = "CatalogDb is stopping";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (itemIds.empty() || itemIds.size() > kMaxMetadataByIdRows) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::ConfigurationFailed;
            rejected.message = "metadata-by-ID query exceeds bounded limit";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.cancelled = true;
            rejected.error = CatalogDbErrorCategory::Superseded;
            rejected.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(rejected));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            rejected.error = CatalogDbErrorCategory::OpenFailed;
            rejected.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(rejected));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0)
            command->metadata.generation = m_generation;
        if (command->metadata.scopeEpoch == 0)
            command->metadata.scopeEpoch = m_requestedEpoch;
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeries(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueReconcile(series, authoritative, metadata, -1);
}

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeriesForTest(
    const std::vector<MediaItem> &series, bool authoritative,
    int failAfterRows)
{
    return enqueueReconcile(series, authoritative, {}, failAfterRows);
}

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

std::future<CatalogDbReconcileResult> CatalogDb::enqueueReconcile(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata, int failAfterRows)
{
    auto command = std::make_shared<ReconcileCommand>();
    command->series = series;
    command->authoritative = authoritative;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    command->failAfterRows = failAfterRows;
    std::future<CatalogDbReconcileResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb reconciliation cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb reconciliation queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_reconcileCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
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

std::future<CatalogDbMediaPageResult> CatalogDb::readMediaPage(
    const std::string &type, int alphabetLetter, std::size_t limit,
    const CatalogDbPageCursor &after, const CatalogDbJobMetadata &metadata,
    CatalogDbMediaPageFilter filter)
{ return enqueueMediaPage(type, alphabetLetter, limit, after, metadata, filter); }

CatalogDbPopulationStatus CatalogDb::populationStatus() const
{ std::lock_guard<std::mutex> lock(m_mutex); return m_populationStatus; }

std::future<CatalogDbMediaPageResult> CatalogDb::enqueueMediaPage(
    const std::string &type, int alphabetLetter, std::size_t limit,
    const CatalogDbPageCursor &after, const CatalogDbJobMetadata &metadata,
    CatalogDbMediaPageFilter filter)
{
    auto command=std::make_shared<MediaPageCommand>(); command->type=type; command->letter=alphabetLetter; command->limit=std::min<std::size_t>(limit, 64); command->filter=filter; command->after=after; command->metadata=metadata;
    auto future=command->result.get_future(); std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping){CatalogDbMediaPageResult r;r.error=CatalogDbErrorCategory::ScopeNotReady;r.message="CatalogDb is stopping";command->result.set_value(std::move(r));return future;}
    if(command->limit==0 || (type!="movie" && type!="show")){CatalogDbMediaPageResult r;r.error=CatalogDbErrorCategory::SqliteError;r.message="invalid media page request";command->result.set_value(std::move(r));return future;}
    if(m_pendingJobs>=kMaxPendingJobs){CatalogDbMediaPageResult r;r.error=CatalogDbErrorCategory::OpenFailed;r.message="CatalogDb page queue is full";command->result.set_value(std::move(r));return future;}
    command->metadata.generation=command->metadata.generation?command->metadata.generation:m_generation; command->metadata.scopeEpoch=command->metadata.scopeEpoch?command->metadata.scopeEpoch:m_requestedEpoch;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    m_mediaPageCommands.push_back(command); ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(
        static_cast<std::uint32_t>(m_pendingJobs));
    m_wake.notify_one(); return future;
}

std::future<CatalogCompatibilityReadResult> CatalogDb::enqueueLibraryRead(
    const CatalogDbJobMetadata &metadata)
{
    auto command=std::make_shared<LibraryReadCommand>(); command->metadata=metadata;
    auto future=command->result.get_future(); std::lock_guard<std::mutex> lock(m_mutex);
    if(m_stopping){CatalogCompatibilityReadResult r;r.error=CatalogDbErrorCategory::ScopeNotReady;r.message="CatalogDb is stopping";command->result.set_value(std::move(r));return future;}
    if(m_pendingJobs>=kMaxPendingJobs){CatalogCompatibilityReadResult r;r.error=CatalogDbErrorCategory::OpenFailed;r.message="CatalogDb library read queue is full";command->result.set_value(std::move(r));return future;}
    command->metadata.generation=command->metadata.generation?command->metadata.generation:m_generation;
    command->metadata.scopeEpoch=command->metadata.scopeEpoch?command->metadata.scopeEpoch:m_requestedEpoch;
    m_libraryReadCommands.push_back(command);++m_pendingJobs; m_wake.notify_one(); return future;
}

std::future<CatalogCompatibilitySeedResult> CatalogDb::enqueueLibrarySeed(
    const CatalogCompatibilitySeedRequest &request,
    const CatalogDbJobMetadata &metadata,
    int failAfterWrites)
{
    auto command = std::make_shared<LibrarySeedCommand>();
    command->request = request;
    command->metadata = metadata;
    command->failAfterWrites = failAfterWrites;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    auto future = command->result.get_future();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) { CatalogCompatibilitySeedResult r; r.error=CatalogDbErrorCategory::ScopeNotReady; r.message="CatalogDb is stopping"; command->result.set_value(std::move(r)); return future; }
    if (m_pendingJobs >= kMaxPendingJobs) { CatalogCompatibilitySeedResult r; r.error=CatalogDbErrorCategory::OpenFailed; r.message="CatalogDb library seed queue is full"; command->result.set_value(std::move(r)); return future; }
    command->metadata.generation = command->metadata.generation ? command->metadata.generation : m_generation;
    command->metadata.scopeEpoch = command->metadata.scopeEpoch ? command->metadata.scopeEpoch : m_requestedEpoch;
    m_librarySeedCommands.push_back(command); ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
    m_wake.notify_one();
    return future;
}

CatalogDbEnqueueResult CatalogDb::enqueueScopedNoopForTest(
    CatalogDbPriority priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedStopping;
    }
    if (!m_scopeConfigured || !m_scopeReady) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedScopeNotReady;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedFull;
    }
    CatalogDbJobMetadata metadata;
    metadata.generation = m_generation;
    metadata.scopeEpoch = m_requestedEpoch;
    m_queues[priorityIndex(priority)].push_back(
        {priority, metadata, telemetryNowIfEnabled()});
    ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(
        static_cast<uint32_t>(m_pendingJobs));
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
                    || !m_syncStateCommands.empty()
                    || !m_librarySeedCommands.empty()
                    || !m_libraryReadCommands.empty()
                    || !m_mediaPageCommands.empty()
                    || !m_mediaPageUpsertCommands.empty()
                    || !m_topLevelSyncCommands.empty()
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
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processScopeCommand(std::move(command));
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_testCommands.empty()) {
            std::shared_ptr<TestCommand> command = std::move(m_testCommands.front());
            m_testCommands.pop_front();
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processTestCommand(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_queryCommands.empty()) {
            std::shared_ptr<QueryCommand> command = std::move(m_queryCommands.front());
            m_queryCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processHierarchyQuery(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_writeCommands.empty()) {
            std::shared_ptr<HierarchyWriteCommand> command =
                std::move(m_writeCommands.front());
            m_writeCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processHierarchyWrite(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_reconcileCommands.empty()) {
            std::shared_ptr<ReconcileCommand> command =
                std::move(m_reconcileCommands.front());
            m_reconcileCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processReconcile(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_syncStateCommands.empty()) {
            std::shared_ptr<SyncStateCommand> command =
                std::move(m_syncStateCommands.front());
            m_syncStateCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processSyncState(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_librarySeedCommands.empty()) {
            auto command = std::move(m_librarySeedCommands.front());
            m_librarySeedCommands.pop_front(); --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true; performanceTelemetry().setCatalogDbActive(true); lock.unlock();
            processLibrarySeed(command);
            lock.lock(); performanceTelemetry().setCatalogDbActive(false); m_runningJob = false; m_idle.notify_all();
            continue;
        }
        if (!m_libraryReadCommands.empty()) {
            auto command=std::move(m_libraryReadCommands.front()); m_libraryReadCommands.pop_front(); --m_pendingJobs;
            m_runningJob=true; performanceTelemetry().setCatalogDbActive(true); lock.unlock(); processLibraryRead(command);
            lock.lock(); performanceTelemetry().setCatalogDbActive(false); m_runningJob=false; m_idle.notify_all(); continue;
        }
        if (!m_mediaPageCommands.empty()) {
            auto command=std::move(m_mediaPageCommands.front()); m_mediaPageCommands.pop_front(); --m_pendingJobs;
            m_runningJob=true; performanceTelemetry().setCatalogDbActive(true); lock.unlock(); processMediaPage(command);
            lock.lock(); performanceTelemetry().setCatalogDbActive(false); m_runningJob=false; m_idle.notify_all(); continue;
        }
        if (!m_mediaPageUpsertCommands.empty()) {
            auto command=std::move(m_mediaPageUpsertCommands.front()); m_mediaPageUpsertCommands.pop_front(); --m_pendingJobs;
            m_runningJob=true; performanceTelemetry().setCatalogDbActive(true); lock.unlock(); processMediaPageUpsert(command);
            lock.lock(); performanceTelemetry().setCatalogDbActive(false); m_runningJob=false; m_idle.notify_all(); continue;
        }
        if (!m_topLevelSyncCommands.empty()) {
            auto command = std::move(m_topLevelSyncCommands.front());
            m_topLevelSyncCommands.pop_front(); --m_pendingJobs;
            m_runningJob = true; performanceTelemetry().setCatalogDbActive(true);
            lock.unlock(); processTopLevelSync(command);
            lock.lock(); performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false; m_idle.notify_all(); continue;
        }

        Job job = takeNextJobLocked();
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
        m_runningJob = true;
        performanceTelemetry().setCatalogDbActive(true);
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
        performanceTelemetry().setCatalogDbActive(false);
        const uint64_t completedUs = telemetryNowIfEnabled();
        if (job.enqueuedMonotonicUs != 0 && completedUs >= job.enqueuedMonotonicUs)
            performanceTelemetry().recordCatalogDbQueueWait(
                completedUs - job.enqueuedMonotonicUs);
        if (disposition == CatalogDbJobDisposition::Completed)
            performanceTelemetry().addCatalogDbCompleted();
        else if (disposition == CatalogDbJobDisposition::Cancelled
                 || disposition == CatalogDbJobDisposition::Superseded)
            performanceTelemetry().addCatalogDbCancelled();
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
        char line[256];
        std::snprintf(line, sizeof(line),
                      "scope_command_started epoch=%llu scope_hash=%s kind=%u",
                      static_cast<unsigned long long>(command.epoch),
                      command.scopeKey.empty() ? "none" : command.scopeKey.c_str(),
                      static_cast<unsigned>(command.kind));
        catalogDiagnostic(line);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch) {
            catalogDiagnostic("scope_command_skipped reason=stale_epoch");
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
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
    }

    closeConnection();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_topLevelSyncGeneration = 0;
        m_activeTopLevelSyncGeneration = 0;
    }

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
        catalogFinalDiagnostic(
            false,
            command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbScopeStatus::InvalidIdentity
                : CatalogDbScopeStatus::Unconfigured,
            command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbErrorCategory::InvalidIdentity
                : CatalogDbErrorCategory::None,
            CatalogDbOpenState::NotAttempted);
        m_idle.notify_all();
        return;
    }

    const CatalogDbMigrationState migrationState =
        inspectMigrationState(command.scopeKey);
    const bool bootstrapNeeded = !migrationState.finalPresent;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_migrationState = migrationState;
        }
    }
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "state epoch=%llu scope_hash=%s db_dir=%s final_present=%d migrating_present=%d bootstrap_needed=%d decision=%s(%u) path_error=%d",
                      static_cast<unsigned long long>(command.epoch),
                      command.scopeKey.c_str(), scopeDirectory(command.scopeKey).c_str(),
                      migrationState.finalPresent ? 1 : 0,
                      migrationState.migratingPresent ? 1 : 0,
                      bootstrapNeeded ? 1 : 0,
                      migrationDecisionName(migrationState.decision),
                      static_cast<unsigned>(migrationState.decision),
                      migrationState.pathError ? 1 : 0);
        catalogDiagnostic(line);
    }
    if (!bootstrapNeeded) {
        catalogDiagnostic("bootstrap_job_skipped reason=final_database_present");
    } else if (migrationState.pathError) {
        catalogDiagnostic("bootstrap_job_skipped reason=path_error");
    } else {
        catalogDiagnostic("bootstrap_job_started");
    }

    const bool opened = openConnection(command);
    const CatalogDbMigrationState completedState =
        inspectMigrationState(command.scopeKey);
    bool attempted = false;
    bool succeeded = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        attempted = m_migrationState.attempted;
        succeeded = m_migrationState.succeeded;
        if (command.epoch == m_requestedEpoch) {
            m_migrationState = completedState;
            m_migrationState.attempted = attempted;
            m_migrationState.succeeded = succeeded;
        }
    }
    if (bootstrapNeeded) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_migrationState.attempted = true;
            m_migrationState.succeeded = opened;
        }
        catalogDiagnostic(std::string("bootstrap_job_completed success=")
                          + (opened ? "1" : "0"));
    }
    // Do not enqueue download-catalog maintenance during scope opening.
    // It is not needed to publish online Home readiness, and an unobserved
    // filesystem scan can otherwise keep the CatalogDb worker in disk sleep
    // until application shutdown. Offline projection reads durable download
    // state outside the CatalogDb worker.
    m_idle.notify_all();
}

void CatalogDb::processHierarchyQuery(
    const std::shared_ptr<QueryCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbHierarchyResult result;
    result.workerOwned = true;
    const uint64_t operationStartUs = telemetryNowIfEnabled();
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (operationStartUs != 0 && endUs >= operationStartUs)
            telemetry.recordCatalogDbQuery(endUs - operationStartUs);
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (command->kind != QueryCommand::Kind::MediaItemsByIds
        && command->kind != QueryCommand::Kind::DeleteMediaItemsByIds
        && command->parentId.empty()) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "hierarchy query requires a parent ID";
        finish();
        return;
    }
    if ((command->kind == QueryCommand::Kind::MediaItemsByIds
         || command->kind == QueryCommand::Kind::DeleteMediaItemsByIds)
        && (command->itemIds.empty()
            || command->itemIds.size() > kMaxMetadataByIdRows)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "metadata-by-ID query exceeds bounded limit";
        finish();
        return;
    }

    enum class QueryState : unsigned char {
        Valid,
        Cancelled,
        Superseded,
    };
    auto state = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            return QueryState::Cancelled;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            return QueryState::Superseded;
        }
        return QueryState::Valid;
    };
    auto rejectState = [&](QueryState queryState) {
        result.error = CatalogDbErrorCategory::Superseded;
        if (queryState == QueryState::Cancelled) {
            result.cancelled = true;
            result.message = "CatalogDb query cancelled";
        } else {
            result.superseded = true;
            result.message = "CatalogDb query superseded";
        }
    };
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        rejectState(queryState);
        finish();
        return;
    }

    if (command->kind == QueryCommand::Kind::DeleteMediaItemsByIds) {
        std::string deleteSql = "DELETE FROM media_items WHERE id IN (";
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            if (i) deleteSql += ",";
            deleteSql += "?" + std::to_string(i + 1);
        }
        deleteSql += ")";
        sqlite3_stmt *deleteItems = nullptr;
        auto existing = m_statements.find("media_items_delete_by_ids");
        if (existing != m_statements.end()) {
            deleteItems = existing->second;
        } else if (sqlite3_prepare_v2(m_db, deleteSql.c_str(), -1,
                                      &deleteItems, nullptr) != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            finish();
            return;
        } else {
            m_statements.emplace("media_items_delete_by_ids", deleteItems);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_preparedStatementCount = m_statements.size();
            }
        }
        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        reset(deleteItems);
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            const int rc = i < command->itemIds.size()
                ? sqlite3_bind_text(deleteItems, static_cast<int>(i + 1),
                                    command->itemIds[i].c_str(), -1,
                                    SQLITE_TRANSIENT)
                : sqlite3_bind_null(deleteItems, static_cast<int>(i + 1));
            if (rc != SQLITE_OK) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                reset(deleteItems);
                finish();
                return;
            }
        }
        auto rollback = [&] {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
        };
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)
            || sqlite3_step(deleteItems) != SQLITE_DONE) {
            if (result.message.empty()) result.message = sqlite3_errmsg(m_db);
            result.error = CatalogDbErrorCategory::SqliteError;
            reset(deleteItems);
            rollback();
            finish();
            return;
        }
        reset(deleteItems);
        if (const QueryState queryState = state();
            queryState != QueryState::Valid) {
            rollback();
            rejectState(queryState);
            finish();
            return;
        }
        if (!exec(m_db, "COMMIT;", result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            rollback();
            finish();
            return;
        }
        result.success = true;
        finish();
        return;
    }

    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
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
    sqlite3_stmt *query = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    sqlite3_stmt *selectGenres = nullptr;
    sqlite3_stmt *selectImageTags = nullptr;
    const bool metadataByIds =
        command->kind == QueryCommand::Kind::MediaItemsByIds;
    const char *queryName = metadataByIds
        ? "media_items_by_ids" : command->kind == QueryCommand::Kind::Seasons
            ? "hierarchy_get_seasons" : "hierarchy_get_episodes";
    std::string querySql;
    if (metadataByIds) {
        querySql =
            "SELECT id, kind, title, overview, production_year, "
            "community_rating, etag, played, progress, "
            "playback_position_ticks, index_number, parent_index_number, "
            "runtime_ticks, series_name, series_id, season_id, art_r, "
            "art_g, art_b FROM media_items WHERE id IN (";
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            if (i) querySql += ",";
            querySql += "?" + std::to_string(i + 1);
        }
        querySql += ") ORDER BY id";
    } else if (command->kind == QueryCommand::Kind::Seasons) {
        querySql =
            "SELECT id, kind, title, overview, production_year, "
            "community_rating, etag, played, progress, "
            "playback_position_ticks, index_number, parent_index_number, "
            "runtime_ticks, series_name, series_id, season_id, art_r, "
            "art_g, art_b FROM media_items WHERE series_id=?1 AND kind=3 "
            "ORDER BY index_number, title, id";
    } else {
        querySql =
            "SELECT id, kind, title, overview, production_year, "
            "community_rating, etag, played, progress, "
            "playback_position_ticks, index_number, parent_index_number, "
            "runtime_ticks, series_name, series_id, season_id, art_r, "
            "art_g, art_b FROM media_items WHERE season_id=?1 AND kind=4 "
            "ORDER BY index_number, title, id";
    }
    if (!prepareCached(queryName, querySql.c_str(), query)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)
        || !prepareCached(
               "media_item_genres_select",
               "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
               "ORDER BY ordinal",
               selectGenres)
        || !prepareCached(
               "media_item_image_tags_select",
               "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
               "ORDER BY image_type",
               selectImageTags)) {
        finish();
        return;
    }

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    reset(query);
    if (metadataByIds) {
        for (std::size_t i = 0; i < kMaxMetadataByIdRows; ++i) {
            const int rc = i < command->itemIds.size()
                ? sqlite3_bind_text(query, static_cast<int>(i + 1),
                                    command->itemIds[i].c_str(), -1,
                                    SQLITE_TRANSIENT)
                : sqlite3_bind_null(query, static_cast<int>(i + 1));
            if (rc != SQLITE_OK) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                reset(query);
                finish();
                return;
            }
        }
    } else if (sqlite3_bind_text(query, 1, command->parentId.c_str(), -1,
                                 SQLITE_TRANSIENT) != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(query);
            finish();
            return;
    }
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag,
        selectGenres, selectImageTags};
    for (;;) {
        if (const QueryState queryState = state();
            queryState != QueryState::Valid) {
            reset(query);
            rejectState(queryState);
            finish();
            return;
        }
        const int rc = sqlite3_step(query);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(query);
            finish();
            return;
        }
        if (result.items.size() >= kMaxHierarchyQueryRows) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query result exceeds bounded limit";
            reset(query);
            finish();
            return;
        }
        MediaItem item;
        MediaItemSqlError readError = MediaItemSqlError::None;
        if (!readMediaItemScalars(query, item, readError)
            || !readMediaItemCollections(collections, item, readError)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "hierarchy query row decode failed";
            reset(query);
            finish();
            return;
        }
        result.items.push_back(std::move(item));
    }
    reset(query);
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        result.items.clear();
        rejectState(queryState);
        finish();
        return;
    }
    result.success = true;
    finish();
}

void CatalogDb::processReconcile(
    const std::shared_ptr<ReconcileCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbReconcileResult result;
    result.workerOwned = true;
    result.authoritative = command->authoritative;
    uint64_t transactionStartUs = 0;
    bool transactionActive = false;
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (transactionActive && endUs >= transactionStartUs) {
            telemetry.recordCatalogDbTransaction(endUs - transactionStartUs);
            transactionActive = false;
        }
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!command->authoritative) {
        result.skipped = true;
        result.success = true;
        result.message = "non-authoritative reconciliation skipped";
        finish();
        return;
    }
    if (!validateReconcileInput(command->series, result.message)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        finish();
        return;
    }

    auto stateIsValid = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation cancelled";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation superseded";
            return false;
        }
        return true;
    };
    if (!stateIsValid()) {
        finish();
        return;
    }

    if (!exec(m_db,
              "CREATE TEMP TABLE IF NOT EXISTS catalog_reconcile_ids("
              "id TEXT PRIMARY KEY NOT NULL);",
              result.message)) {
        result.error = CatalogDbErrorCategory::SqliteError;
        finish();
        return;
    }
    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
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
    sqlite3_stmt *upsert = nullptr;
    sqlite3_stmt *clearIds = nullptr;
    sqlite3_stmt *stageId = nullptr;
    sqlite3_stmt *deleteAbsent = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    if (!prepareCached(
            "hierarchy_upsert_item",
            "INSERT INTO media_items("
            "id, kind, title, overview, production_year, community_rating,"
            "etag, played, progress, playback_position_ticks, index_number,"
            "parent_index_number, runtime_ticks, series_name, series_id,"
            "season_id, art_r, art_g, art_b) "
            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
            "?13, ?14, ?15, ?16, ?17, ?18, ?19) "
            "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, "
            "title=excluded.title, overview=excluded.overview, "
            "production_year=excluded.production_year, "
            "community_rating=excluded.community_rating, etag=excluded.etag, "
            "played=excluded.played, progress=excluded.progress, "
            "playback_position_ticks=excluded.playback_position_ticks, "
            "index_number=excluded.index_number, "
            "parent_index_number=excluded.parent_index_number, "
            "runtime_ticks=excluded.runtime_ticks, "
            "series_name=excluded.series_name, series_id=excluded.series_id, "
            "season_id=excluded.season_id, art_r=excluded.art_r, "
            "art_g=excluded.art_g, art_b=excluded.art_b",
            upsert)
        || !prepareCached("reconcile_clear_ids",
                          "DELETE FROM catalog_reconcile_ids", clearIds)
        || !prepareCached(
               "reconcile_stage_id",
               "INSERT INTO catalog_reconcile_ids(id) VALUES(?1)", stageId)
        || !prepareCached(
               "reconcile_delete_absent",
               "DELETE FROM media_items WHERE kind=2 AND id NOT IN "
               "(SELECT id FROM catalog_reconcile_ids)",
               deleteAbsent)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)) {
        finish();
        return;
    }
    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    auto rollback = [&] {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
    };
    auto executeNoParameter = [&](sqlite3_stmt *statement) {
        reset(statement);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        return ok;
    };
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag};
    auto writeSeries = [&](const MediaItem &item) {
        if (!stateIsValid()) {
            return false;
        }
        MediaItemSqlError error = MediaItemSqlError::None;
        bool existed = false;
        if (performanceTelemetry().enabledFast()
            && !mediaItemRowExists(m_db, item.id, existed, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            return false;
        }
        reset(upsert);
        const bool ok = bindMediaItemScalars(upsert, item, error)
            && sqlite3_step(upsert) == SQLITE_DONE
            && maintainOrganizationalSortKey(m_db, item, result.message);
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            reset(upsert);
            return false;
        }
        performanceTelemetry().addCatalogDbRows(existed ? 0u : 1u,
                                                existed ? 1u : 0u, 0u);
        reset(upsert);
        if (!replaceMediaItemCollections(collections, item, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "reconciliation collection write failed";
            return false;
        }
        ++result.seriesUpserted;
        if (command->failAfterRows >= 0
            && result.seriesUpserted >= static_cast<std::size_t>(
                                            command->failAfterRows)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected reconciliation failure";
            return false;
        }
        reset(stageId);
        const bool staged = sqlite3_bind_text(stageId, 1, item.id.c_str(), -1,
                                              SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(stageId) == SQLITE_DONE;
        if (!staged) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(stageId);
        return staged;
    };

    const uint64_t beginUs = telemetryNowIfEnabled();
    const bool began = exec(m_db, "BEGIN IMMEDIATE;", result.message);
    if (began) {
        transactionStartUs = beginUs;
        transactionActive = true;
    }
    if (!began
        || !executeNoParameter(clearIds)) {
        rollback();
        finish();
        return;
    }
    for (const auto &item : command->series) {
        if (!writeSeries(item)) {
            rollback();
            finish();
            return;
        }
    }
    if (!stateIsValid()) {
        rollback();
        finish();
        return;
    }
    reset(deleteAbsent);
    if (sqlite3_step(deleteAbsent) != SQLITE_DONE) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(deleteAbsent);
        rollback();
        finish();
        return;
    }
    result.seriesDeleted = static_cast<std::size_t>(sqlite3_changes(m_db));
    performanceTelemetry().addCatalogDbRows(
        0, 0, static_cast<uint32_t>(result.seriesDeleted));
    reset(deleteAbsent);
    const uint64_t commitStartUs = telemetryNowIfEnabled();
    if (!exec(m_db, "COMMIT;", result.message)) {
        rollback();
        finish();
        return;
    }
    transactionActive = false;
    if (commitStartUs != 0) {
        const uint64_t commitEndUs = telemetryNowIfEnabled();
        if (commitEndUs >= commitStartUs)
            performanceTelemetry().recordCatalogDbCommit(commitEndUs - commitStartUs);
    }
    if (transactionStartUs != 0) {
        const uint64_t transactionEndUs = telemetryNowIfEnabled();
        if (transactionEndUs >= transactionStartUs)
            performanceTelemetry().recordCatalogDbTransaction(
                transactionEndUs - transactionStartUs);
    }
    result.success = true;
    finish();
}

void CatalogDb::processTopLevelSync(
    const std::shared_ptr<TopLevelSyncCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbTopLevelSyncResult result; result.workerOwned = true;
    std::lock_guard<std::mutex> lock(m_mutex);
    result.generation = command->generation;
    if (!m_scopeConfigured || !m_scopeReady || command->metadata.scopeEpoch == 0
        || command->metadata.scopeEpoch != m_requestedEpoch
        || command->generation == 0) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "top-level sync scope is not current";
        command->result.set_value(std::move(result)); return;
    }
    if (!m_db) {
        result.error = CatalogDbErrorCategory::OpenFailed;
        result.message = "top-level sync database is not open";
        command->result.set_value(std::move(result)); return;
    }
    std::string error;
    const char *const createSql[] = {
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_views (generation INTEGER NOT NULL, id TEXT NOT NULL, name TEXT NOT NULL, collection_type TEXT NOT NULL, ordinal INTEGER NOT NULL, PRIMARY KEY(generation, id))",
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_membership (generation INTEGER NOT NULL, view_id TEXT NOT NULL, item_id TEXT NOT NULL, ordinal INTEGER NOT NULL, PRIMARY KEY(generation, view_id, item_id))",
        "CREATE TEMP TABLE IF NOT EXISTS top_level_sync_active (generation INTEGER NOT NULL PRIMARY KEY)",
    };
    for (const char *sql : createSql) {
        if (!exec(m_db, sql, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
    }
    if (command->begin) {
        if (!exec(m_db, "DELETE FROM top_level_sync_views; DELETE FROM top_level_sync_membership; DELETE FROM top_level_sync_active;", error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(m_db,
                "INSERT INTO top_level_sync_active(generation) VALUES(?1)",
                -1, &statement, nullptr) != SQLITE_OK
            || sqlite3_bind_int64(statement, 1,
                static_cast<sqlite3_int64>(command->generation)) != SQLITE_OK
            || sqlite3_step(statement) != SQLITE_DONE) {
            if (statement) sqlite3_finalize(statement);
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            command->result.set_value(std::move(result)); return;
        }
        sqlite3_finalize(statement);
        m_topLevelSyncGeneration = command->generation;
        m_activeTopLevelSyncGeneration = command->generation;
        result.success = true;
    } else if (command->finalize) {
        if (m_activeTopLevelSyncGeneration != command->generation) {
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "top-level sync generation is not active";
            command->result.set_value(std::move(result)); return;
        }
        const std::string copyViews = "INSERT INTO library_views(id,name,collection_type,ordinal) SELECT id,name,collection_type,ordinal FROM top_level_sync_views WHERE generation=" + std::to_string(command->generation) + " ORDER BY ordinal,id;";
        const std::string copyMembership = "INSERT INTO library_membership(view_id,item_id,ordinal) SELECT view_id,item_id,ordinal FROM top_level_sync_membership WHERE generation=" + std::to_string(command->generation) + " ORDER BY view_id,ordinal,item_id;";
        if (!exec(m_db, "BEGIN IMMEDIATE;", error)
            || !exec(m_db, "DELETE FROM library_membership; DELETE FROM library_views;", error)
            || !exec(m_db, copyViews.c_str(), error)
            || !exec(m_db, copyMembership.c_str(), error)
            || !exec(m_db, "COMMIT;", error)) {
            std::string ignored; exec(m_db, "ROLLBACK;", ignored);
            result.error = CatalogDbErrorCategory::SqliteError; result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        exec(m_db, "DELETE FROM top_level_sync_views; DELETE FROM top_level_sync_membership; DELETE FROM top_level_sync_active;", error);
        m_activeTopLevelSyncGeneration = 0;
        result.success = true;
    } else if (m_activeTopLevelSyncGeneration == command->generation) {
        m_activeTopLevelSyncGeneration = 0;
        if (!exec(m_db, "DELETE FROM top_level_sync_active", error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = error;
            command->result.set_value(std::move(result)); return;
        }
        result.success = true;
    } else {
        result.success = true;
    }
    command->result.set_value(std::move(result));
}

void CatalogDb::processMediaPage(const std::shared_ptr<MediaPageCommand> &command)
{
    CatalogDbMediaPageResult result; result.workerOwned=true;
    const std::uint64_t queryStartUs = telemetryNowIfEnabled();
    catalogDiagnostic("read_media_page_dequeued");
    auto finish = [&] {
        const std::uint64_t endUs = telemetryNowIfEnabled();
        if (queryStartUs != 0 && endUs >= queryStartUs)
            performanceTelemetry().recordCatalogDbQuery(endUs - queryStartUs);
        if (command->enqueuedMonotonicUs != 0
            && endUs >= command->enqueuedMonotonicUs) {
            performanceTelemetry().recordCatalogDbQueueWait(
                endUs - command->enqueuedMonotonicUs);
        }
        if (result.cancelled || result.superseded) {
            catalogDiagnostic("read_media_page_cancelled");
            performanceTelemetry().addCatalogDbCancelled();
        } else if (result.success) {
            catalogDiagnostic("read_media_page_ready");
            performanceTelemetry().addCatalogDbCompleted();
        } else {
            catalogDiagnostic("read_media_page_failed");
            performanceTelemetry().addCatalogDbFailed();
        }
        command->result.set_value(std::move(result));
    };
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeReady) {
            result.superseded=true;
            result.error=CatalogDbErrorCategory::Superseded;
            finish();
            return;
        }
    }
    sqlite3_stmt *statement=nullptr, *genres=nullptr, *tags=nullptr,
                 *memberships=nullptr;
    const std::string indexName = "idx_media_" + command->type + "_sort";
    const std::string animeFilter = command->filter == CatalogDbMediaPageFilter::Anime
        ? " AND (EXISTS (SELECT 1 FROM library_membership anime_membership "
          "JOIN library_views anime_views ON anime_views.id=anime_membership.view_id "
          "WHERE anime_membership.item_id=media_items.id "
          "AND anime_views.collection_type='tvshows' "
          "AND lower(anime_views.name) LIKE '%anime%') "
          "OR EXISTS (SELECT 1 FROM item_genres anime_genres "
          "WHERE anime_genres.item_id=media_items.id "
          "AND lower(anime_genres.genre)='anime'))"
        : "";
    const std::string sql =
        "SELECT id,kind,title,overview,production_year,community_rating,"
        "etag,played,progress,playback_position_ticks,index_number,"
        "parent_index_number,runtime_ticks,series_name,series_id,season_id,"
        "art_r,art_g,art_b FROM media_items INDEXED BY " + indexName
        + " WHERE kind=?1 AND "
        "EXISTS (SELECT 1 FROM library_membership "
        "JOIN library_views ON library_views.id=library_membership.view_id "
        "WHERE library_membership.item_id=media_items.id "
        "AND library_views.collection_type=?10) AND "
        "1=1" + animeFilter + " AND "
        "(?2 < 0 OR (organizational_sort_key>=?3 AND "
        "organizational_sort_key<?4)) AND (?5=0 OR "
        "(organizational_sort_key,title,id)>(?6,?7,?8)) "
        "ORDER BY organizational_sort_key,title,id LIMIT ?9";
    if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(
               m_db,
               "SELECT ordinal,genre FROM item_genres WHERE item_id=?1 "
               "ORDER BY ordinal", -1, &genres, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(
               m_db,
               "SELECT image_type,tag FROM item_image_tags WHERE item_id=?1 "
               "ORDER BY image_type", -1, &tags, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(
               m_db,
               "SELECT library_views.id,library_views.name,"
               "library_views.collection_type FROM library_membership "
               "JOIN library_views ON library_views.id=library_membership.view_id "
               "WHERE library_membership.item_id=?1 "
               "ORDER BY library_views.ordinal,library_views.id",
               -1, &memberships, nullptr) != SQLITE_OK) {
        result.error=CatalogDbErrorCategory::SqliteError;
        result.message=sqlite3_errmsg(m_db);
        sqlite3_finalize(statement); sqlite3_finalize(genres);
        sqlite3_finalize(tags); sqlite3_finalize(memberships);
        finish();
        return;
    }
    const std::string lower = command->letter >= 0
        ? std::string(1, static_cast<char>('a' + command->letter)) : "";
    const std::string upper = command->letter >= 0
        ? std::string(1, static_cast<char>('a' + command->letter + 1)) : "";
    sqlite3_bind_int(statement,1,command->type=="movie"?1:2);
    sqlite3_bind_int(statement,2,command->letter);
    sqlite3_bind_text(statement,3,lower.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(statement,4,upper.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int(statement,5,command->after.valid?1:0);
    sqlite3_bind_text(statement,6,command->after.sortKey.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(statement,7,command->after.title.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(statement,8,command->after.id.c_str(),-1,SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement,9,static_cast<sqlite3_int64>(command->limit+1));
    sqlite3_bind_text(statement,10,command->type=="movie"?"movies":"tvshows",-1,SQLITE_STATIC);
    const MediaItemCollectionStatements collections{
        nullptr, nullptr, nullptr, nullptr, genres, tags};
    while (sqlite3_step(statement)==SQLITE_ROW) {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled=true;
            break;
        }
        MediaItem item;
        MediaItemSqlError e=MediaItemSqlError::None;
        if (!readMediaItemScalars(statement,item,e)
            || !readMediaItemCollections(collections,item,e)) {
            result.error=CatalogDbErrorCategory::SqliteError;
            result.message=sqlite3_errmsg(m_db);
            break;
        }
        sqlite3_reset(memberships); sqlite3_clear_bindings(memberships);
        sqlite3_bind_text(memberships, 1, item.id.c_str(), -1, SQLITE_TRANSIENT);
        int membershipRc = SQLITE_DONE;
        while ((membershipRc = sqlite3_step(memberships)) == SQLITE_ROW) {
            CatalogDbMediaPageMembership membership;
            const char *viewId = reinterpret_cast<const char *>(
                sqlite3_column_text(memberships, 0));
            const char *viewName = reinterpret_cast<const char *>(
                sqlite3_column_text(memberships, 1));
            const char *collectionType = reinterpret_cast<const char *>(
                sqlite3_column_text(memberships, 2));
            membership.viewId = viewId ? viewId : "";
            membership.viewName = viewName ? viewName : "";
            membership.collectionType = collectionType ? collectionType : "";
            result.membershipsByItem[item.id].push_back(std::move(membership));
        }
        if (membershipRc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        if (result.items.size()<command->limit)
            result.items.push_back(std::move(item));
        else
            result.hasMore=true;
    }
    if (!result.items.empty()) {
        const auto &item=result.items.back();
        result.next.sortKey=catalog::organizationalSortKey(item.title);
        result.next.title=item.title;
        result.next.id=item.id;
        result.next.valid=true;
    }
    sqlite3_finalize(statement); sqlite3_finalize(genres); sqlite3_finalize(tags);
    sqlite3_finalize(memberships);
    if (!result.cancelled&&result.error==CatalogDbErrorCategory::None)
        result.success=true;
    finish();
}

void CatalogDb::processLibraryRead(const std::shared_ptr<LibraryReadCommand> &command)
{
    // This whole-snapshot path is retained for compatibility/tests. The
    // normal Home flow renders bounded library pages and receives ephemeral
    // rail responses from Jellyfin; it must not depend on home_items.
    CatalogCompatibilityReadResult result; result.workerOwned=true;
    { std::lock_guard<std::mutex> lock(m_mutex); if(command->metadata.generation!=m_generation || (command->metadata.scopeEpoch && (command->metadata.scopeEpoch!=m_requestedEpoch||!m_scopeConfigured||!m_scopeReady))){result.superseded=true;result.error=CatalogDbErrorCategory::Superseded;command->result.set_value(std::move(result));return;} }
    sqlite3_stmt *views=nullptr,*items=nullptr,*home=nullptr,*sg=nullptr,*st=nullptr;
    const char *cols="id,kind,title,overview,production_year,community_rating,etag,played,progress,playback_position_ticks,index_number,parent_index_number,runtime_ticks,series_name,series_id,season_id,art_r,art_g,art_b";
    if(sqlite3_prepare_v2(m_db,"SELECT id,name,collection_type FROM library_views ORDER BY ordinal,id",-1,&views,nullptr)!=SQLITE_OK || sqlite3_prepare_v2(m_db,(std::string("SELECT ")+cols+" FROM media_items JOIN library_membership ON media_items.id=library_membership.item_id WHERE view_id=? ORDER BY library_membership.ordinal,library_membership.item_id").c_str(),-1,&items,nullptr)!=SQLITE_OK || sqlite3_prepare_v2(m_db,(std::string("SELECT ")+cols+",row_kind FROM media_items JOIN home_items ON media_items.id=home_items.item_id ORDER BY row_kind,ordinal,item_id").c_str(),-1,&home,nullptr)!=SQLITE_OK || sqlite3_prepare_v2(m_db,"SELECT ordinal,genre FROM item_genres WHERE item_id=? ORDER BY ordinal",-1,&sg,nullptr)!=SQLITE_OK || sqlite3_prepare_v2(m_db,"SELECT image_type,tag FROM item_image_tags WHERE item_id=? ORDER BY image_type",-1,&st,nullptr)!=SQLITE_OK){result.error=CatalogDbErrorCategory::SqliteError;result.message=sqlite3_errmsg(m_db);sqlite3_finalize(views);sqlite3_finalize(items);sqlite3_finalize(home);sqlite3_finalize(sg);sqlite3_finalize(st);command->result.set_value(std::move(result));return;}
    const MediaItemCollectionStatements collections{nullptr,nullptr,nullptr,nullptr,sg,st};
    auto cancelled = [&] { return command->metadata.cancellation
        && command->metadata.cancellation->load(); };
    while(sqlite3_step(views)==SQLITE_ROW){
        if (cancelled()) { result.cancelled=true; break; }
        CachedLibraryView view;view.id=(const char*)sqlite3_column_text(views,0);view.name=(const char*)sqlite3_column_text(views,1);view.collectionType=(const char*)sqlite3_column_text(views,2);sqlite3_reset(items);sqlite3_clear_bindings(items);sqlite3_bind_text(items,1,view.id.c_str(),-1,SQLITE_TRANSIENT);
        while(sqlite3_step(items)==SQLITE_ROW){
            if (cancelled()) { result.cancelled=true; break; }
            MediaItem item;MediaItemSqlError e=MediaItemSqlError::None;if(!readMediaItemScalars(items,item,e)||!readMediaItemCollections(collections,item,e)){result.error=CatalogDbErrorCategory::SqliteError;result.message=sqlite3_errmsg(m_db);sqlite3_finalize(views);sqlite3_finalize(items);sqlite3_finalize(sg);sqlite3_finalize(st);command->result.set_value(std::move(result));return;}view.items.push_back(std::move(item));
        }
        if (result.cancelled) break;
        if(view.collectionType=="tvshows")result.snapshot.shows.push_back(std::move(view));else result.snapshot.movies.push_back(std::move(view));
    }
    if (!result.cancelled) while(sqlite3_step(home)==SQLITE_ROW){
        if (cancelled()) { result.cancelled=true; break; }
        const char *kind=(const char*)sqlite3_column_text(home,19);MediaItem item;MediaItemSqlError e=MediaItemSqlError::None;if(!readMediaItemScalars(home,item,e)||!readMediaItemCollections(collections,item,e)){result.error=CatalogDbErrorCategory::SqliteError;result.message=sqlite3_errmsg(m_db);sqlite3_finalize(views);sqlite3_finalize(items);sqlite3_finalize(home);sqlite3_finalize(sg);sqlite3_finalize(st);command->result.set_value(std::move(result));return;}if(kind&&std::string(kind)=="continue_watching")result.snapshot.continueWatching.push_back(std::move(item));else if(kind&&std::string(kind)=="recently_added")result.snapshot.recentlyAdded.push_back(std::move(item));
    }
    if (cancelled()) result.cancelled=true;
    sqlite3_finalize(views);sqlite3_finalize(items);sqlite3_finalize(home);sqlite3_finalize(sg);sqlite3_finalize(st);if(!result.cancelled)result.success=true;command->result.set_value(std::move(result));
}

void CatalogDb::processLibrarySeed(
    const std::shared_ptr<LibrarySeedCommand> &command)
{
    CatalogCompatibilitySeedResult result; result.workerOwned = true;
    auto stale = [&] { std::lock_guard<std::mutex> lock(m_mutex); return
        command->metadata.generation != m_generation ||
        (command->metadata.scopeEpoch != 0 && (command->metadata.scopeEpoch != m_requestedEpoch || !m_scopeConfigured || !m_scopeReady)); };
    if (command->metadata.cancellation && command->metadata.cancellation->load()) { result.cancelled=true; result.error=CatalogDbErrorCategory::Superseded; result.message="library seed cancelled"; command->result.set_value(std::move(result)); return; }
    if (stale()) { result.superseded=true; result.error=CatalogDbErrorCategory::Superseded; result.message="library seed superseded"; command->result.set_value(std::move(result)); return; }
    std::map<std::string, MediaItem> canonical;
    std::vector<MediaItem> items;
    auto collect = [&](const std::vector<CachedLibraryView> &views) {
        for (const auto &view : views) for (const auto &item : view.items) {
            if (item.id.empty() || (item.type != "movie" && item.type != "show")) return false;
            auto found = canonical.find(item.id);
            if (found != canonical.end()) { if (!mediaItemsEquivalentForCatalog(found->second, item)) return false; }
            else { canonical.emplace(item.id, item); items.push_back(item); }
        } return true;
    };
    if (!collect(command->request.snapshot.movies) || !collect(command->request.snapshot.shows)) { result.error=CatalogDbErrorCategory::CorruptOrIo; result.message="invalid library snapshot"; command->result.set_value(std::move(result)); return; }
    auto collectHome = [&](const std::vector<MediaItem> &row) { for (const auto &item : row) { if(item.id.empty() || (item.type!="movie"&&item.type!="show"&&item.type!="episode")) return false; auto found=canonical.find(item.id); if(found!=canonical.end()){if(!mediaItemsEquivalentForCatalog(found->second,item))return false;} else {canonical.emplace(item.id,item);items.push_back(item);} } return true; };
    if(!collectHome(command->request.snapshot.continueWatching)||!collectHome(command->request.snapshot.recentlyAdded)){result.error=CatalogDbErrorCategory::CorruptOrIo;result.message="invalid Home snapshot";command->result.set_value(std::move(result));return;}
    auto execSeed = [&](const std::string &sql) { char *err=nullptr; int rc=sqlite3_exec(m_db,sql.c_str(),nullptr,nullptr,&err); if(rc!=SQLITE_OK){result.message=err?err:sqlite3_errmsg(m_db); sqlite3_free(err); return false;} return true; };
    if (!execSeed("BEGIN IMMEDIATE;")) { result.error=CatalogDbErrorCategory::SqliteError; command->result.set_value(std::move(result)); return; }
    auto rollback = [&] { char *e=nullptr; sqlite3_exec(m_db,"ROLLBACK;",nullptr,nullptr,&e); sqlite3_free(e); };
    sqlite3_stmt *upsert=nullptr;
    sqlite3_stmt *deleteGenres=nullptr,*insertGenre=nullptr,*deleteTags=nullptr,*insertTag=nullptr;
    const char *sql="INSERT INTO media_items(id,kind,title,overview,production_year,community_rating,etag,played,progress,playback_position_ticks,index_number,parent_index_number,runtime_ticks,series_name,series_id,season_id,art_r,art_g,art_b) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET kind=excluded.kind,title=excluded.title,overview=excluded.overview,production_year=excluded.production_year,community_rating=excluded.community_rating,etag=excluded.etag,played=excluded.played,progress=excluded.progress,playback_position_ticks=excluded.playback_position_ticks,index_number=excluded.index_number,parent_index_number=excluded.parent_index_number,runtime_ticks=excluded.runtime_ticks,series_name=excluded.series_name,series_id=excluded.series_id,season_id=excluded.season_id,art_r=excluded.art_r,art_g=excluded.art_g,art_b=excluded.art_b";
    if (sqlite3_prepare_v2(m_db,sql,-1,&upsert,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"DELETE FROM item_genres WHERE item_id=?",-1,&deleteGenres,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"INSERT INTO item_genres(item_id,ordinal,genre) VALUES(?,?,?)",-1,&insertGenre,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"DELETE FROM item_image_tags WHERE item_id=?",-1,&deleteTags,nullptr)!=SQLITE_OK
        || sqlite3_prepare_v2(m_db,"INSERT INTO item_image_tags(item_id,image_type,tag) VALUES(?,?,?)",-1,&insertTag,nullptr)!=SQLITE_OK) { result.error=CatalogDbErrorCategory::SqliteError; result.message=sqlite3_errmsg(m_db); rollback(); command->result.set_value(std::move(result)); return; }
    const MediaItemCollectionStatements collections{deleteGenres,insertGenre,deleteTags,insertTag};
    int writes=0;
    for (const auto &item : items) { MediaItemSqlError e=MediaItemSqlError::None; sqlite3_reset(upsert); sqlite3_clear_bindings(upsert); if(!bindMediaItemScalars(upsert,item,e)||sqlite3_step(upsert)!=SQLITE_DONE||!maintainOrganizationalSortKey(m_db,item,result.message)||!replaceMediaItemCollections(collections,item,e)){result.error=CatalogDbErrorCategory::SqliteError;result.message=sqlite3_errmsg(m_db);sqlite3_finalize(upsert);sqlite3_finalize(deleteGenres);sqlite3_finalize(insertGenre);sqlite3_finalize(deleteTags);sqlite3_finalize(insertTag);rollback();command->result.set_value(std::move(result));return;} ++result.itemsUpserted; if(command->failAfterWrites>=0&&++writes>=command->failAfterWrites){result.error=CatalogDbErrorCategory::SqliteError;result.message="injected library seed failure";sqlite3_finalize(upsert);sqlite3_finalize(deleteGenres);sqlite3_finalize(insertGenre);sqlite3_finalize(deleteTags);sqlite3_finalize(insertTag);rollback();command->result.set_value(std::move(result));return;} }
    sqlite3_finalize(upsert); sqlite3_finalize(deleteGenres); sqlite3_finalize(insertGenre); sqlite3_finalize(deleteTags); sqlite3_finalize(insertTag);
    // This is the compatibility-only writer for legacy home_items.
    // Normal top-level synchronization never enters this path.
    if (!execSeed("DELETE FROM library_membership; DELETE FROM library_views; DELETE FROM home_items;")) { result.error=CatalogDbErrorCategory::SqliteError; rollback(); command->result.set_value(std::move(result)); return; }
    auto addView = [&](const CachedLibraryView &v, int ordinal) { sqlite3_stmt *s=nullptr; if(sqlite3_prepare_v2(m_db,"INSERT INTO library_views(id,name,collection_type,ordinal) VALUES(?,?,?,?)",-1,&s,nullptr)!=SQLITE_OK)return false; sqlite3_bind_text(s,1,v.id.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,v.name.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,v.collectionType.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,4,ordinal);bool ok=sqlite3_step(s)==SQLITE_DONE;sqlite3_finalize(s);if(!ok)return false; for(size_t i=0;i<v.items.size();++i){if(!execSeed("INSERT INTO library_membership(view_id,item_id,ordinal) VALUES('"+v.id+"','"+v.items[i].id+"',"+std::to_string(i)+")"))return false;} ++result.viewsWritten;return true; };
    for(size_t i=0;i<command->request.snapshot.movies.size();++i) if(!addView(command->request.snapshot.movies[i],i)){result.error=CatalogDbErrorCategory::SqliteError;rollback();command->result.set_value(std::move(result));return;}
    for(size_t i=0;i<command->request.snapshot.shows.size();++i) if(!addView(command->request.snapshot.shows[i],i+command->request.snapshot.movies.size())){result.error=CatalogDbErrorCategory::SqliteError;rollback();command->result.set_value(std::move(result));return;}
    auto addHome=[&](const char *kind,const std::vector<MediaItem>&v){for(size_t i=0;i<v.size();++i)if(!execSeed("INSERT INTO home_items(row_kind,item_id,ordinal) VALUES('"+std::string(kind)+"','"+v[i].id+"',"+std::to_string(i)+")"))return false;result.homeItemsWritten+=v.size();return true;};
    if(!addHome("continue_watching",command->request.snapshot.continueWatching)||!addHome("recently_added",command->request.snapshot.recentlyAdded)||stale()||!execSeed("COMMIT;")){result.error=CatalogDbErrorCategory::Superseded;rollback();command->result.set_value(std::move(result));return;}
    result.success=true; command->result.set_value(std::move(result));
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

    case kSchemaDiagnosticsOperation: {
        std::set<std::string> objects;
        if (!collectObjects(m_db, objects, result.message)
            || !scalarInt(m_db, "PRAGMA application_id;",
                          result.applicationId, result.message)
            || !scalarInt(m_db, "PRAGMA user_version;", result.userVersion,
                          result.message)
            || !scalar(m_db,
                       "SELECT singleton_id FROM sync_state "
                       "WHERE singleton_id=1;",
                       result.sentinel, result.message)
            || !schemaConstraints(m_db, result.foreignKeyCascade,
                                  result.checkConstraints, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        const std::set<std::string> expectedObjects = {
            "index:idx_media_movie_sort",
            "index:idx_media_show_sort",
            "index:idx_media_season_order",
            "index:idx_media_series_kind_order",
            "table:hierarchy_state",
            "table:item_genres",
            "table:item_image_tags",
            "table:media_items",
            "table:sync_state",
            "index:idx_home_items_row_order",
            "index:idx_library_membership_item",
            "index:idx_library_membership_view_order",
            "index:idx_library_views_ordinal",
            "table:home_items",
            "table:library_membership",
            "table:library_views",
        };
        result.exactSchema = objects == expectedObjects;
        result.singletonSeeded = result.sentinel == "1";
        result.success = result.exactSchema
            && result.foreignKeyCascade && result.checkConstraints
            && result.singletonSeeded
            && result.applicationId == kCatalogApplicationId
            && result.userVersion == 3;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "schema v3 diagnostics did not match the contract";
        }
        break;
    }

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

    case kMediaItemCodecOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT OR REPLACE INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19)",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)) {
            break;
        }

        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto roundTrip = [&](const MediaItem &expected,
                             bool nullableRelationships) {
            MediaItemSqlError bindError = MediaItemSqlError::None;
            if (!bindMediaItemScalars(insert, expected, bindError)) {
                result.message = "MediaItem scalar bind failed";
                reset(insert);
                return false;
            }
            if (nullableRelationships
                && (sqlite3_bind_null(insert, 15) != SQLITE_OK
                    || sqlite3_bind_null(insert, 16) != SQLITE_OK)) {
                result.message = "MediaItem nullable bind failed";
                reset(insert);
                return false;
            }
            const int insertRc = sqlite3_step(insert);
            reset(insert);
            if (insertRc != SQLITE_DONE) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                return false;
            }

            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                result.message = "MediaItem scalar lookup bind failed";
                reset(select);
                return false;
            }
            const int selectRc = sqlite3_step(select);
            if (selectRc != SQLITE_ROW) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk) {
                result.message = expected.id + ": scalar read failed";
                return false;
            }
            if (!sameMediaItemScalars(expected, actual)) {
                result.message = expected.id + ": scalar comparison failed";
                return false;
            }
            return true;
        };

        MediaItem series;
        series.id = "__task08_series__";
        series.type = "show";
        series.title = "Full Series";
        series.overview = "Series overview";
        series.year = 2024;
        series.rating = 8.25f;
        series.etag = "series-etag";
        series.played = true;
        series.progress = 0.75f;
        series.playbackPositionTicks = 123456789;
        series.indexNumber = 7;
        series.parentIndexNumber = 8;
        series.runTimeTicks = 987654321;
        series.seriesName = "Series name";
        series.artR = 10;
        series.artG = 20;
        series.artB = 30;

        MediaItem season = series;
        season.id = "__task08_season__";
        season.type = "season";
        season.seriesId = series.id;
        season.title = "Full Season";

        MediaItem episode = season;
        episode.id = "__task08_episode__";
        episode.type = "episode";
        episode.seasonId = season.id;
        episode.title = "Full Episode";

        MediaItem movie = series;
        movie.id = "__task08_movie__";
        movie.type = "movie";
        movie.seriesName.clear();
        movie.seriesId.clear();
        movie.seasonId.clear();
        movie.title = "Full Movie";

        result.codecPopulatedKinds = roundTrip(series, false)
            && roundTrip(season, false) && roundTrip(episode, false)
            && roundTrip(movie, false);

        MediaItem defaults;
        defaults.id = "__task08_defaults__";
        defaults.type = "movie";
        result.codecDefaults = roundTrip(defaults, true);
        result.codecNullableRelationships = result.codecDefaults
            && defaults.seriesId.empty() && defaults.seasonId.empty();

        MediaItem boundaries;
        boundaries.id = "__task08_boundaries__";
        boundaries.type = "episode";
        boundaries.year = std::numeric_limits<int>::min();
        boundaries.rating = 10.0f;
        boundaries.progress = 1.0f;
        boundaries.playbackPositionTicks = std::numeric_limits<long long>::min();
        boundaries.indexNumber = std::numeric_limits<int>::min();
        boundaries.parentIndexNumber = std::numeric_limits<int>::max();
        boundaries.runTimeTicks = std::numeric_limits<long long>::max();
        boundaries.artR = 0;
        boundaries.artG = 1;
        boundaries.artB = 255;
        result.codecBoundaries = roundTrip(boundaries, false);

        MediaItem invalidKind;
        invalidKind.id = "__task08_invalid_kind__";
        invalidKind.type = "unknown";
        MediaItemSqlError invalidError = MediaItemSqlError::None;
        MediaItemSqlError invalidReadError = MediaItemSqlError::None;
        result.codecInvalidKind = !bindMediaItemScalars(insert, invalidKind,
                                                         invalidError)
            && invalidError == MediaItemSqlError::InvalidKind
            && mediaItemKindFromSql(99, invalidReadError).empty()
            && invalidReadError == MediaItemSqlError::InvalidKind;
        reset(insert);

        MediaItem missingId;
        missingId.type = "movie";
        MediaItemSqlError missingError = MediaItemSqlError::None;
        result.codecMissingId = !bindMediaItemScalars(insert, missingId,
                                                       missingError)
            && missingError == MediaItemSqlError::MissingId;
        reset(insert);

        exec(m_db, "DELETE FROM media_items WHERE id LIKE '__task08_%';",
             result.message);
        result.success = result.codecPopulatedKinds && result.codecDefaults
            && result.codecBoundaries && result.codecInvalidKind
            && result.codecMissingId && result.codecNullableRelationships;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem scalar codec checks failed";
            }
        }
        break;
    }

    case kMediaItemCollectionsOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        sqlite3_stmt *deleteGenres = nullptr;
        sqlite3_stmt *insertGenre = nullptr;
        sqlite3_stmt *deleteImageTags = nullptr;
        sqlite3_stmt *insertImageTag = nullptr;
        sqlite3_stmt *selectGenres = nullptr;
        sqlite3_stmt *selectImageTags = nullptr;
        sqlite3_stmt *deleteItem = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT OR REPLACE INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19)",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)
            || !prepareNamed(
                "media_item_genres_delete",
                "DELETE FROM item_genres WHERE item_id=?1", deleteGenres)
            || !prepareNamed(
                "media_item_genre_insert",
                "INSERT INTO item_genres(item_id, ordinal, genre) "
                "VALUES(?1, ?2, ?3)", insertGenre)
            || !prepareNamed(
                "media_item_image_tags_delete",
                "DELETE FROM item_image_tags WHERE item_id=?1",
                deleteImageTags)
            || !prepareNamed(
                "media_item_image_tag_insert",
                "INSERT INTO item_image_tags(item_id, image_type, tag) "
                "VALUES(?1, ?2, ?3)", insertImageTag)
            || !prepareNamed(
                "media_item_genres_select",
                "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
                "ORDER BY ordinal", selectGenres)
            || !prepareNamed(
                "media_item_image_tags_select",
                "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
                "ORDER BY image_type", selectImageTags)
            || !prepareNamed(
                "media_item_delete",
                "DELETE FROM media_items WHERE id=?1", deleteItem)) {
            break;
        }

        const MediaItemCollectionStatements collections{
            deleteGenres, insertGenre, deleteImageTags, insertImageTag,
            selectGenres, selectImageTags};
        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto rollback = [&] {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
        };
        auto insertAndRead = [&](const MediaItem &expected) {
            if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
                return false;
            }
            MediaItemSqlError error = MediaItemSqlError::None;
            reset(insert);
            if (!bindMediaItemScalars(insert, expected, error)
                || sqlite3_step(insert) != SQLITE_DONE) {
                result.message = sqlite3_errmsg(m_db);
                reset(insert);
                rollback();
                return false;
            }
            reset(insert);
            if (!replaceMediaItemCollections(collections, expected, error)) {
                result.message = "MediaItem collection replacement failed";
                rollback();
                return false;
            }
            if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                return false;
            }

            reset(select);
            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK
                || sqlite3_step(select) != SQLITE_ROW) {
                result.message = sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk || !readMediaItemCollections(collections, actual,
                                                     readError)) {
                result.message = "MediaItem collection read failed";
                return false;
            }
            return mediaItemsEquivalentForCatalog(expected, actual);
        };

        MediaItem item;
        item.id = "__task09_item__";
        item.type = "movie";
        item.title = "Collection Fixture";

        result.collectionsZero = insertAndRead(item);

        item.genres = {"Drama", "Science Fiction", "Thriller"};
        item.genre = item.genres.front();
        item.imageTags = {{"Backdrop", "backdrop-tag"},
                          {"Primary", "primary-tag"},
                          {"Thumb", "thumb-tag"}};
        result.collectionsMultiple = insertAndRead(item);

        MediaItem updated = item;
        updated.genres = {"Updated"};
        updated.genre = updated.genres.front();
        updated.imageTags.clear();
        result.collectionsUpdateRemoval = insertAndRead(updated);
        if (result.collectionsUpdateRemoval) {
            result.collectionsUpdateRemoval = updated.imageTags.empty()
                && updated.genres.size() == 1
                && updated.genre == updated.genres.front();
        }

        result.collectionsParity = result.collectionsZero
            && result.collectionsMultiple && result.collectionsUpdateRemoval;

        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
            result.collectionsDeleteCascade = false;
        } else {
            reset(deleteItem);
            const bool deleteOk = sqlite3_bind_text(
                                    deleteItem, 1, item.id.c_str(), -1,
                                    SQLITE_TRANSIENT) == SQLITE_OK
                && sqlite3_step(deleteItem) == SQLITE_DONE;
            reset(deleteItem);
            if (!deleteOk) {
                result.message = sqlite3_errmsg(m_db);
                rollback();
                result.collectionsDeleteCascade = false;
            } else if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                result.collectionsDeleteCascade = false;
            } else {
                MediaItem deleted;
                deleted.id = item.id;
                MediaItemSqlError readError = MediaItemSqlError::None;
                const bool childrenEmpty = readMediaItemCollections(
                    collections, deleted, readError)
                    && deleted.genres.empty() && deleted.genre.empty()
                    && deleted.imageTags.empty();
                std::int64_t remaining = 1;
                const bool rowGone = scalarInt(
                    m_db,
                    "SELECT COUNT(*) FROM media_items "
                    "WHERE id='__task09_item__';",
                    remaining, result.message) && remaining == 0;
                result.collectionsDeleteCascade = childrenEmpty && rowGone;
            }
        }

        result.success = result.collectionsParity
            && result.collectionsDeleteCascade;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem collection checks failed";
            }
        }
        break;
    }

    case kSeedHierarchyQueryOperation: {
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)
            || !exec(m_db,
                     "DELETE FROM media_items WHERE id LIKE '__task10_%';",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title) VALUES"
                     "('__task10_series__', 2, 'Target Series'),"
                     "('__task10_other_series__', 2, 'Other Series');",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, series_id, "
                     "index_number) VALUES"
                     "('__task10_season_z__', 3, 'Zed', "
                     "'__task10_series__', 1),"
                     "('__task10_season_b__', 3, 'Beta', "
                     "'__task10_series__', 2),"
                     "('__task10_season_a__', 3, 'Alpha', "
                     "'__task10_series__', 1);",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, season_id, "
                     "series_id, index_number) VALUES"
                     "('__task10_episode_z__', 4, 'Zed Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_a__', 4, 'Alpha Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_b__', 4, 'Beta Episode', "
                     "'__task10_season_a__', '__task10_series__', 2);",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        for (int index = 0; index < 200; ++index) {
            const std::string statement =
                "INSERT INTO media_items(id, kind, title, series_id, "
                "index_number) VALUES('__task10_other_season_"
                + std::to_string(index) + "', 3, 'Other', "
                "'__task10_other_series__', " + std::to_string(index) + ");";
            if (!exec(m_db, statement.c_str(), result.message)) {
                std::string ignored;
                exec(m_db, "ROLLBACK;", ignored);
                break;
            }
            if (index == 199) {
                result.hierarchyFixture = true;
            }
        }
        if (!result.hierarchyFixture) {
            break;
        }
        if (!exec(m_db,
                  "INSERT INTO item_genres(item_id, ordinal, genre) "
                  "VALUES('__task10_season_a__', 0, 'Drama'),"
                  "('__task10_season_a__', 1, 'Mystery');",
                  result.message)
            || !exec(m_db,
                     "INSERT INTO item_image_tags(item_id, image_type, tag) "
                     "VALUES('__task10_season_a__', 'Primary', 'season-tag'),"
                     "('__task10_episode_a__', 'Primary', 'episode-tag');",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            result.hierarchyFixture = false;
            break;
        }

        auto planUsesIndex = [&](const char *sql, const char *indexName) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr)
                != SQLITE_OK) {
                return false;
            }
            bool found = false;
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const unsigned char *detail = sqlite3_column_text(statement, 3);
                if (detail && std::string(reinterpret_cast<const char *>(detail))
                                  .find(indexName) != std::string::npos) {
                    found = true;
                }
            }
            sqlite3_finalize(statement);
            return found;
        };
        result.hierarchyIndexes = planUsesIndex(
            "EXPLAIN QUERY PLAN SELECT id FROM media_items "
            "WHERE series_id='__task10_series__' AND kind=3 "
            "ORDER BY index_number, title, id;",
            "idx_media_series_kind_order")
            && planUsesIndex(
                "EXPLAIN QUERY PLAN SELECT id FROM media_items "
                "WHERE season_id='__task10_season_a__' AND kind=4 "
                "ORDER BY index_number, title, id;",
                "idx_media_season_order");
        if (!exec(m_db, "COMMIT;", result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        result.success = result.hierarchyFixture && result.hierarchyIndexes;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query index plan check failed";
        }
        break;
    }

    case kClearHierarchyQueryOperation:
        result.success = exec(
            m_db, "DELETE FROM media_items WHERE id LIKE '__task10_%';",
            result.message);
        break;

    case kMediaPageQueryPlanOperation: {
        const std::size_t separator = command->value.find(':');
        if (separator == std::string::npos) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan arguments";
            break;
        }
        const std::string type = command->value.substr(0, separator);
        int letter = -1;
        try {
            letter = std::stoi(command->value.substr(separator + 1));
        } catch (...) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan alphabet";
            break;
        }
        if ((type != "movie" && type != "show") || letter < -1 || letter > 25) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "invalid media page query-plan arguments";
            break;
        }
        const int kind = type == "movie" ? 1 : 2;
        std::string sql =
            "EXPLAIN QUERY PLAN SELECT id FROM media_items INDEXED BY "
            + ("idx_media_" + type + "_sort")
            + " WHERE kind=" + std::to_string(kind)
            + " AND EXISTS (SELECT 1 FROM library_membership "
              "JOIN library_views ON library_views.id=library_membership.view_id "
              "WHERE library_membership.item_id=media_items.id "
              "AND library_views.collection_type='"
            + (type == "movie" ? "movies" : "tvshows") + "')";
        if (letter >= 0) {
            sql += " AND organizational_sort_key >= '";
            sql += static_cast<char>('a' + letter);
            sql += "' AND organizational_sort_key < '";
            sql += static_cast<char>('a' + letter + 1);
            sql += "'";
        }
        sql += " ORDER BY organizational_sort_key, title, id LIMIT 25;";
        sqlite3_stmt *plan = nullptr;
        if (sqlite3_prepare_v2(m_db, sql.c_str(), -1, &plan, nullptr)
            != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        bool sawTempSort = false;
        while (sqlite3_step(plan) == SQLITE_ROW) {
            const unsigned char *detail = sqlite3_column_text(plan, 3);
            if (!detail) continue;
            const std::string text(reinterpret_cast<const char *>(detail));
            if (text.find("idx_media_" + type + "_sort") != std::string::npos)
                result.mediaPageUsesSortIndex = true;
            if (text.find("USE TEMP B-TREE") != std::string::npos)
                sawTempSort = true;
        }
        sqlite3_finalize(plan);
        result.mediaPageAvoidsTempSort = !sawTempSort;
        result.success = result.mediaPageUsesSortIndex
            && result.mediaPageAvoidsTempSort;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "bounded media page query plan is not indexed";
        }
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

    case kWriteSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_write",
                "INSERT OR REPLACE INTO media_items(id, kind, title) "
                "VALUES('__task06_marker__', 1, ?1)",
                statement)) {
            break;
        }
        const int bindRc = sqlite3_bind_text(statement, 1,
                                             command->value.c_str(), -1,
                                             SQLITE_TRANSIENT);
        const int stepRc = bindRc == SQLITE_OK ? sqlite3_step(statement)
                                                : bindRc;
        reset(statement);
        result.success = stepRc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kReadSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_read",
                "SELECT title FROM media_items "
                "WHERE id='__task06_marker__'",
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

    case kSetSchemaMetadataOperation: {
        const std::size_t separator = command->value.find(':');
        if (separator == std::string::npos) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "invalid schema metadata fixture";
            break;
        }
        const std::string applicationId = command->value.substr(0, separator);
        const std::string userVersion = command->value.substr(separator + 1);
        const std::string applicationIdPragma =
            "PRAGMA application_id = " + applicationId + ";";
        const std::string userVersionPragma =
            "PRAGMA user_version = " + userVersion + ";";
        result.success = exec(m_db, applicationIdPragma.c_str(), result.message)
            && exec(m_db, userVersionPragma.c_str(), result.message);
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
        }
        break;
    }

    case kMigrationRollbackOperation: {
        finalizeStatements();
        std::string migrationError;
        const bool began = exec(m_db, "BEGIN IMMEDIATE;", migrationError);
        const int failedMigration = began
            ? execResult(m_db,
                         "CREATE TABLE catalog_task07_rollback_probe(value TEXT);"
                         "INSERT INTO catalog_task07_missing(value) VALUES(1);",
                         migrationError)
            : SQLITE_ERROR;
        std::string rollbackError;
        const bool rolledBack = began
            && exec(m_db, "ROLLBACK;", rollbackError);
        std::int64_t probeCount = 1;
        const bool inspected = scalarInt(
            m_db,
            "SELECT COUNT(*) FROM sqlite_master WHERE name="
            "'catalog_task07_rollback_probe';",
            probeCount, result.message);
        sqlite3_stmt *probe = nullptr;
        const int prepareRc = sqlite3_prepare_v2(m_db, "SELECT 1", -1,
                                                 &probe, nullptr);
        if (prepareRc == SQLITE_OK && probe) {
            sqlite3_step(probe);
            sqlite3_reset(probe);
            sqlite3_clear_bindings(probe);
            m_statements.emplace("migration_probe", probe);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        result.statementReused = prepareRc == SQLITE_OK && probe != nullptr;
        result.success = failedMigration != SQLITE_OK && rolledBack
            && inspected && probeCount == 0 && result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = migrationError.empty() ? rollbackError
                                                        : migrationError;
            }
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
