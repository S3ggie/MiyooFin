#include "LibraryCoordinator.hpp"
#include "LibraryCoordinatorInternal.hpp"
#include "LibrarySync.hpp"
#include "../net/HttpClient.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <exception>

namespace miyoofin {
namespace library {

using namespace coordinator_detail;

bool LibraryCoordinator::startStartupSync(bool catalogHasRows)
try {
    if (!kCoordinatorStartupSyncEnabled) {
        (void)catalogHasRows;
        uiDiagnostics().log("[LibraryCoordinator] startup_sync_rejected phase=disabled");
        return false;
    }

    // Move any completed prior thread out while holding the mutex, then join
    // it after releasing the mutex. The worker takes this same mutex to
    // publish its result, so joining while locked can deadlock at the handoff.
    std::thread priorThread;
    std::string admissionDiagnostic;
    bool ownsStartupSequence = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        AdmissionRequest request;
        request.requester = OperationKind::Startup;
        std::string reasons;
        if (!serializedOperationAdmittedLocked(request, &reasons)) {
            admissionDiagnostic = "[LibraryCoordinator] startup_sync_rejected phase=admission"
                                  " catalog_has_rows=" +
                                  std::to_string(catalogHasRows ? 1 : 0) + " reasons=" + reasons;
        } else {
            if (m_startupThread.joinable())
                priorThread = std::move(m_startupThread);

            // Reserve the startup slot while the prior thread is being joined so
            // another caller cannot start a second operation in the gap.
            beginOperation(OperationKind::Startup);
            // Only a successful admission may finalize cold-start ownership.
            // Claiming before admission would consume the owner-held
            // reservation and then, on a failed admission caused by another
            // operation occupying the slot, clear it (and the owner token and
            // armed demand) via finishStartupSequenceLocked().  Deferring the
            // claim leaves the reservation, token, and demand intact so a later
            // retry still owns and can release the sequence.
            ownsStartupSequence = claimStartupSequenceLocked(OperationKind::Startup);
            m_startupCancellation = std::make_shared<std::atomic_bool>(false);
        }
    }
    if (!admissionDiagnostic.empty()) {
        // The failed admission did not claim the sequence, so the owner-held
        // reservation and its armed demand remain intact for a retry.
        uiDiagnostics().log(admissionDiagnostic);
        return false;
    }
    if (priorThread.joinable())
        priorThread.join();

    std::string postJoinDiagnostic;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || !m_sync) {
            std::string reasons;
            appendCoordinatorGateReason(reasons, "stopped", m_stopped);
            appendCoordinatorGateReason(reasons, "not_running", !m_running);
            appendCoordinatorGateReason(reasons, "missing_sync", !m_sync);
            postJoinDiagnostic = "[LibraryCoordinator] startup_sync_rejected phase=post_join"
                                 " catalog_has_rows=" +
                                 std::to_string(catalogHasRows ? 1 : 0) + " reasons=" + reasons;
            releaseOperation(OperationKind::Startup);
            if (ownsStartupSequence)
                finishStartupSequenceLocked();
        } else {
            const auto cancellation = m_startupCancellation;
            const auto sync = m_sync;
            const auto db = m_db;
            const auto scopeEpoch = m_scopeEpoch;
            m_startupResult = {};
            m_startupThread =
                std::thread([this, catalogHasRows, cancellation, sync, db, scopeEpoch] {
                    StartupSyncResult result;
                    const auto cancelled = [&] { return cancellation && cancellation->load(); };

                    if (!db || scopeEpoch == 0) {
                        result.error = CatalogDbErrorCategory::ScopeNotReady;
                        result.message = "CatalogDb scope is unavailable";
                    } else if (cancelled()) {
                        result.cancelled = true;
                        result.error = CatalogDbErrorCategory::Superseded;
                        result.message = "startup sync cancelled";
                    } else {
                        CatalogDbJobMetadata metadata;
                        metadata.scopeEpoch = scopeEpoch;
                        metadata.cancellation = cancellation;
                        const auto state = db->readSyncState(false, 0, 0, metadata).get();
                        const std::int64_t nowMs = coordinatorWallClockMs();
                        if (!state.success) {
                            result.mode = StartupSyncMode::FullReconcile;
                        } else {
                            result.generation = state.committedGeneration;
                            result.committedGeneration = state.committedGeneration;
                            result.lastSuccessfulMs = state.lastSuccessfulMs;
                            result.lastReconcileMs = state.lastReconcileMs;
                            if (result.generation > 0)
                                sync->seedTransactionGeneration(result.generation);
                            {
                                std::lock_guard<std::mutex> lock(m_startupMutex);
                                if (result.generation > m_catalogGeneration)
                                    m_catalogGeneration = result.generation;
                                m_lastSuccessfulMs = result.lastSuccessfulMs;
                                m_lastReconcileMs = result.lastReconcileMs;
                            }

                            const bool validCheckpoint =
                                catalogHasRows && state.lastSuccessfulMs > 0 &&
                                result.generation > 0 &&
                                state.lastReconcileMs <= state.lastSuccessfulMs &&
                                nowMs >= state.lastSuccessfulMs;
                            if (!validCheckpoint ||
                                nowMs - state.lastReconcileMs >= 24LL * 60 * 60 * 1000) {
                                result.mode = StartupSyncMode::FullReconcile;
                            } else if (nowMs - state.lastSuccessfulMs < 15LL * 60 * 1000) {
                                result.mode = StartupSyncMode::SkipFresh;
                            } else if (nowMs - state.lastSuccessfulMs < 24LL * 60 * 60 * 1000) {
                                result.mode = StartupSyncMode::DeltaCatchUp;
                            } else {
                                result.mode = StartupSyncMode::FullReconcile;
                            }
                        }

                        if (cancelled()) {
                            result.cancelled = true;
                            result.error = CatalogDbErrorCategory::Superseded;
                            result.message = "startup sync cancelled";
                        } else if (result.mode == StartupSyncMode::DeltaCatchUp) {
                            std::uint64_t committedGeneration = 0;
                            {
                                std::lock_guard<std::mutex> lock(m_startupMutex);
                                committedGeneration = m_catalogGeneration + 1;
                            }
                            const auto catchUp =
                                sync->catchUpChangedCatalog(state.lastSuccessfulMs, cancellation,
                                                            committedGeneration)
                                    .get();
                            result.success = catchUp.success;
                            result.cancelled = catchUp.cancelled;
                            result.superseded = catchUp.superseded;
                            result.error = catchUp.error;
                            result.message = catchUp.message;
                            result.checkpointMs = catchUp.checkpointMs;
                            if (catchUp.success) {
                                result.committedGeneration = committedGeneration;
                                result.generation = committedGeneration;
                                result.lastSuccessfulMs = catchUp.checkpointMs;
                                std::lock_guard<std::mutex> lock(m_startupMutex);
                                m_catalogGeneration =
                                    std::max(m_catalogGeneration, committedGeneration);
                                m_lastSuccessfulMs = result.lastSuccessfulMs;
                            }
                        } else {
                            // SkipFresh and FullReconcile are both successful policy
                            // decisions.  Home owns only the still-unmigrated full
                            // walk; it cannot run until this operation is complete.
                            result.success = true;
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(m_startupMutex);
                        m_startupResult = std::move(result);
                        markOperationPublicationPending(OperationKind::Startup);
                    }
                    m_startupWake.notify_all();
                });
        }
    }
    if (!postJoinDiagnostic.empty()) {
        uiDiagnostics().log(postJoinDiagnostic);
        return false;
    }
    return true;
} catch (...) {
    // A post-join gate failure or worker-thread construction failure must not
    // leave the cold-start demand armed, or retained live changes would be
    // deferred forever.  Only an admitted owner ever reaches throwing code (a
    // competing request now fails the startup-handoff admission gate and
    // returns before this point), but re-check ownership here so a non-owner can
    // never clear the active sequence's demand on the unwind path.
    std::lock_guard<std::mutex> lock(m_startupMutex);
    const bool ownedStartupSlot = currentOperationKind() == OperationKind::Startup;
    releaseOperation(OperationKind::Startup);
    // Only finalize the sequence when this startup actually claimed it (the
    // handler cannot see the try body's local flag).  A pre-claim construction
    // failure leaves m_startupSequenceClaimed false and must preserve the
    // owner-held reservation/demand for a retry.
    if (ownedStartupSlot && m_startupSequenceClaimed)
        finishStartupSequenceLocked();
    throw;
}

bool LibraryCoordinator::takeStartupSyncResult(StartupSyncResult& result)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        took = takeStartupSyncResultLocked(result);
    }
    if (took)
        m_startupWake.notify_all();
    return took;
}

bool LibraryCoordinator::takeStartupSyncResultLocked(StartupSyncResult& result)
{
    if (currentOperationKind() != OperationKind::Startup ||
        currentOperationPhase() != OperationPhase::PublicationPending)
        return false;
    result = std::move(m_startupResult);
    releaseOperation(OperationKind::Startup);
    // The cold-start sequence continues into a full population exactly when
    // Home will request one: a successful FullReconcile policy decision, or a
    // DeltaCatchUp that failed (not cancelled/superseded) and falls back to a
    // full walk.  A startup that could not decide (for example a missing db or
    // scope, which leaves the default FullReconcile mode with success=false)
    // never guarantees that population will follow, so it must not retain the
    // demand and strand retained live changes.  A cancelled or superseded
    // startup is likewise terminal, as is every other decision (SkipFresh,
    // successful DeltaCatchUp).
    const bool populationExpected =
        !result.cancelled && !result.superseded &&
        ((result.mode == StartupSyncMode::FullReconcile && result.success) ||
         (result.mode == StartupSyncMode::DeltaCatchUp && !result.success));
    if (populationExpected) {
        // Startup's worker ownership ends, but the sequence does not: Home
        // still owns the cold-start handoff until it requests and claims the
        // full population.  Track that handoff separately from an unclaimed
        // base reservation so only the intended FullPopulation continuation can
        // consume it; a competing startup/full admission cannot steal the owner
        // token and clear Home's demand when it fails.  Home's destructor can
        // still relinquish an abandoned handoff.
        m_startupSequenceClaimed = false;
        m_startupHandoffPending = true;
        markStartupPopulationDemandLocked();
    } else {
        finishStartupSequenceLocked();
    }
    return true;
}

WaitStatus LibraryCoordinator::waitStartupSyncResult(StartupSyncResult& result,
                                                     const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_startupMutex);
    for (;;) {
        // Publication wins over cancellation or stop, including when the
        // notification and teardown race with this consumer.
        if (takeStartupSyncResultLocked(result))
            return WaitStatus::Ready;
        if (m_stopped)
            return WaitStatus::Stopped;
        const bool startupExecuting = currentOperationKind() == OperationKind::Startup &&
                                      currentOperationPhase() == OperationPhase::Executing;
        const bool cancelled = (consumerCancellation && consumerCancellation->load()) ||
                               (m_startupCancellation && m_startupCancellation->load());
        if (cancelled) {
            // Cancellation requests the worker's terminal publication.  Keep
            // consuming through that handoff so startup does not leave a
            // ready result blocking the next serialized operation.
            if (startupExecuting) {
                m_startupWake.wait(lock);
                continue;
            }
            return WaitStatus::Cancelled;
        }
        if (!startupExecuting)
            return WaitStatus::InvalidRequest;
        m_startupWake.wait(lock);
    }
}

void LibraryCoordinator::cancelStartupSync() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_startupCancellation) {
            cancelled = true;
            m_startupCancellation->store(true);
        }
    }
    if (cancelled)
        m_startupWake.notify_all();
}

} // namespace library
} // namespace miyoofin
