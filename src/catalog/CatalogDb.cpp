#include "CatalogDb.hpp"
#include "CatalogDbInternal.hpp"

#include "CatalogCompatibility.hpp"
#include "../data/MediaItem.hpp"
#include "MediaItemSql.hpp"
#include "CatalogDbSchema.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
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
// Static startup timeline markers retained across the CatalogDb query split: read_media_page_dequeued, read_media_page_ready.
using namespace catalog_db_internal;

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
#ifdef MIYOOFIN_TEST_BUILD
        m_testCommands.clear();
#endif // MIYOOFIN_TEST_BUILD
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

CatalogDbPopulationStatus CatalogDb::populationStatus() const
{ std::lock_guard<std::mutex> lock(m_mutex); return m_populationStatus; }

std::future<CatalogCompatibilitySeedResult> CatalogDb::enqueueLibrarySeed(
    const CatalogCompatibilitySeedRequest &request,
    const CatalogDbJobMetadata &metadata,
    CatalogDbFailureSpec injection)
{
    auto command = std::make_shared<LibrarySeedCommand>();
    command->request = request;
    command->metadata = metadata;
#ifdef MIYOOFIN_TEST_BUILD
    command->injection = injection;
#else
    (void)injection;
#endif // MIYOOFIN_TEST_BUILD
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

void CatalogDb::workerLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] {
#ifdef MIYOOFIN_TEST_BUILD
            const bool pausedForTest = m_pausedForTest;
            const bool hasTestCommands = !m_testCommands.empty();
#else
            const bool pausedForTest = false;
            const bool hasTestCommands = false;
#endif // MIYOOFIN_TEST_BUILD
            return m_stopping || (!pausedForTest
                && (!m_scopeCommands.empty() || hasTestCommands
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

#ifdef MIYOOFIN_TEST_BUILD
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
#endif // MIYOOFIN_TEST_BUILD

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

} // namespace miyoofin