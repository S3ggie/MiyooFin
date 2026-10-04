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

bool LibraryCoordinator::requestLiveChange(const JellyfinLibraryChangeBatch& batch)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || !m_sync)
        return false;
    const bool accepted = m_liveChangeRequests.push(batch);
    m_liveChangeWake.notify_one();
    return accepted;
}

bool LibraryCoordinator::takeLiveChangeResult(LiveLibraryChangeResult& result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_liveChangeResult)
        return false;
    result = std::move(*m_liveChangeResult);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
    releaseOperation(OperationKind::LiveChange);
    m_liveChangeWake.notify_one();
    return true;
}

void LibraryCoordinator::cancelLiveChange() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_liveChangeCancellation)
        m_liveChangeCancellation->store(true);
}

void LibraryCoordinator::discardLiveChangeResults() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    m_liveChangeResult.reset();
    m_liveChangeActive.reset();
    releaseOperation(OperationKind::LiveChange);
    m_liveChangeWake.notify_one();
}

void LibraryCoordinator::liveChangeWorker()
{
    for (;;) {
        JellyfinLibraryChangeBatch batch;
        LiveChangeIdentity identity;
        std::shared_ptr<std::atomic_bool> cancellation;
        {
            std::unique_lock<std::mutex> lock(m_startupMutex);
            // The event receiver writes directly to LibrarySync's bounded
            // queue. Drain it here, never on Home's SDL thread, before
            // checking the serialized top-level gates.  If the coordinator
            // queue is full, push() may have coalesced only part of the
            // batch.  Preserve that loss as an explicit catch-up barrier and
            // stop popping the source until the barrier is consumed.
            if (!m_liveChangeDrainPendingCatchUp && m_sync) {
                JellyfinLibraryChangeBatch incoming;
                while (m_sync->takeLiveChange(incoming)) {
                    if (m_liveChangeRequests.push(incoming))
                        continue;
                    m_liveChangeRequests.markCatchUpRequired();
                    m_liveChangeDrainPendingCatchUp = true;
                    break;
                }
            }
            if (m_liveChangeStop)
                return;
            AdmissionRequest admission;
            admission.requester = OperationKind::LiveChange;
            const bool serializedSlotOpen = serializedOperationAdmittedLocked(admission, nullptr);
            // Cold-start precedence: while Home's startup/full-population
            // sequence is expected or underway, do not dequeue or claim the
            // serialized slot.  The retained batch is applied once the demand
            // clears (the coordinator notifies m_liveChangeWake).
            const bool startupDemandDeferred = m_startupPopulationDemand;
            if (startupDemandDeferred || !serializedSlotOpen || !m_liveChangeRequests.pop(batch)) {
#ifdef MIYOOFIN_TEST_BUILD
                if (startupDemandDeferred) {
                    // Deterministic seam: let a test observe that the worker
                    // reached the demand gate without claiming the slot.
                    ++m_liveChangeDemandDeferralsForTest;
                    m_liveChangeTestWake.notify_all();
                }
#endif
                m_liveChangeWake.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            if (batch.catchUpRequired)
                m_liveChangeDrainPendingCatchUp = true;
            if (coordinatorLiveChangeIsEmpty(batch))
                continue;

            identity.worker = ++m_liveChangeWorker;
            identity.generation = m_catalogGeneration;
            identity.request = ++m_liveChangeRequest;
            m_liveChangeActive = identity;
            beginOperation(OperationKind::LiveChange);
            m_liveChangeCancellation = std::make_shared<std::atomic_bool>(false);
            cancellation = m_liveChangeCancellation;
        }

        LiveLibraryChangeResult result;
        result.catchUpRequired = batch.catchUpRequired;
        result.userDataChanged = batch.userDataChanged;
        std::uint64_t committedGeneration = identity.generation;
        std::uint64_t operationGeneration = 0;
        bool catchUpSucceeded = !batch.catchUpRequired;
        const auto cancelled = [&] { return cancellation && cancellation->load(); };
        const auto nextCommittedGeneration = [&] {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            return m_catalogGeneration + 1;
        };
        const auto commitGeneration = [&](std::uint64_t generation) {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            m_catalogGeneration = std::max(m_catalogGeneration, generation);
            committedGeneration = m_catalogGeneration;
            return committedGeneration;
        };
        const auto nextOperationGeneration = [&] {
            if (operationGeneration == 0)
                operationGeneration = nextCommittedGeneration();
            return operationGeneration;
        };
        const auto publish = [&](LiveLibraryChangeResult value) {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            // Discarding a result or accepting a newer worker identity makes
            // this publication stale. It must never be consumed by the next
            // live batch.  Releasing the slot lets the worker drain the next
            // queued batch after a consumer discarded this operation.
            if (!m_liveChangeActive || !(*m_liveChangeActive == identity)) {
                releaseOperation(OperationKind::LiveChange);
                m_liveChangeWake.notify_one();
                return;
            }
            if (m_liveChangeResult)
                return;
            value.generation = std::max(value.generation, committedGeneration);
            m_liveChangeResult = std::move(value);
#ifdef MIYOOFIN_TEST_BUILD
            // Deterministic seam: wake a test waiting for the deferred batch to
            // be applied after the cold-start demand cleared.
            m_liveChangeTestWake.notify_all();
#endif
            // The mutation is durable; release the global serialized slot now
            // and retain the immutable terminal result for Home.  Same-kind
            // admission is gated on the pending result so a later live batch
            // cannot overwrite it.
            releaseOperation(OperationKind::LiveChange);
            m_liveChangeWake.notify_one();
        };

        try {
            if (!m_db || m_scopeEpoch == 0) {
                result.error = CatalogDbErrorCategory::ScopeNotReady;
                result.message = "CatalogDb scope is unavailable";
            } else if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "live library change cancelled";
            } else if (batch.catchUpRequired) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                const auto state = m_db->readSyncState(false, 0, 0, metadata).get();
                if (!state.success) {
                    result.error = state.error;
                    result.message = state.message;
                    result.lastSuccessfulMs = state.lastSuccessfulMs;
                    result.lastReconcileMs = state.lastReconcileMs;
                } else {
                    result.lastSuccessfulMs = state.lastSuccessfulMs;
                    result.lastReconcileMs = state.lastReconcileMs;
                    if (state.committedGeneration > committedGeneration) {
                        std::lock_guard<std::mutex> lock(m_startupMutex);
                        if (state.committedGeneration > m_catalogGeneration)
                            m_catalogGeneration = state.committedGeneration;
                        committedGeneration = m_catalogGeneration;
                    }
                    if (cancelled()) {
                        result.cancelled = true;
                        result.error = CatalogDbErrorCategory::Superseded;
                        result.message = "live library change cancelled";
                    } else if (state.lastSuccessfulMs > 0) {
                        const auto catchUpGeneration = nextOperationGeneration();
                        const auto catchUp =
                            m_sync
                                ->catchUpChangedCatalog(state.lastSuccessfulMs, cancellation,
                                                        catchUpGeneration)
                                .get();
                        if (!catchUp.success) {
                            result.cancelled = catchUp.cancelled;
                            result.superseded = catchUp.superseded;
                            result.error = catchUp.error;
                            result.message = catchUp.message;
                        } else {
                            catchUpSucceeded = true;
                            result.checkpointMs = catchUp.checkpointMs;
                            result.lastSuccessfulMs = catchUp.checkpointMs;
                            result.generation = commitGeneration(catchUpGeneration);
                            result.committedGeneration = result.generation;
                            {
                                std::lock_guard<std::mutex> lock(m_startupMutex);
                                m_lastSuccessfulMs = result.lastSuccessfulMs;
                            }
                        }
                    } else {
                        committedGeneration = nextOperationGeneration();
                        const auto transactionGeneration = m_sync->nextTransactionGeneration();
                        const auto reconciled =
                            m_sync
                                ->reconcileAuthoritativeMembership(
                                    cancellation, transactionGeneration, committedGeneration)
                                .get();
                        if (!reconciled.success) {
                            result.cancelled = reconciled.cancelled;
                            result.superseded = reconciled.superseded;
                            result.error = reconciled.error;
                            result.message = reconciled.message;
                        } else {
                            result.generation = commitGeneration(committedGeneration);
                            result.committedGeneration = result.generation;
                            const std::int64_t nowMs = coordinatorWallClockMs();
                            // The authoritative membership commit is already
                            // durable. Finish this checkpoint even if the
                            // caller requested cancellation meanwhile.
                            const auto checkpoint =
                                m_sync->writeSyncState(nowMs, nowMs, committedGeneration).get();
                            if (checkpoint.success) {
                                catchUpSucceeded = true;
                                result.checkpointMs = checkpoint.lastSuccessfulMs;
                                result.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                                result.lastReconcileMs = checkpoint.lastReconcileMs;
                                std::lock_guard<std::mutex> lock(m_startupMutex);
                                m_lastSuccessfulMs = result.lastSuccessfulMs;
                                m_lastReconcileMs = result.lastReconcileMs;
                            } else if (result.message.empty()) {
                                result.error = checkpoint.error;
                                result.message = checkpoint.message;
                            }
                        }
                    }
                }
            }

            const bool catchUpFailed = batch.catchUpRequired && !catchUpSucceeded;
            if (!catchUpFailed && result.error == CatalogDbErrorCategory::None &&
                !result.cancelled && !result.superseded) {
                if (batch.itemsAdded.empty() && batch.itemsRemoved.empty() &&
                    batch.itemsUpdated.empty() && !batch.catchUpRequired) {
                    result.success = true;
                } else {
                    auto applied =
                        m_sync->applyLibraryChanges(batch, cancellation, nextOperationGeneration())
                            .get();
                    const bool userDataChanged = result.userDataChanged;
                    const auto priorGeneration = result.generation;
                    result = std::move(applied);
                    result.userDataChanged = userDataChanged;
                    result.generation = std::max(result.generation, priorGeneration);
                    result.catchUpRequired = batch.catchUpRequired || result.catchUpRequired;
                    if (result.success) {
                        result.generation = commitGeneration(nextOperationGeneration());
                        result.committedGeneration = result.generation;
                    }
                }
            }
        } catch (const std::exception& error) {
            result.cancelled = cancelled();
            result.superseded = !result.cancelled;
            result.error = result.cancelled ? CatalogDbErrorCategory::Superseded
                                            : CatalogDbErrorCategory::SqliteError;
            result.message = error.what();
        } catch (...) {
            result.cancelled = cancelled();
            result.superseded = !result.cancelled;
            result.error = result.cancelled ? CatalogDbErrorCategory::Superseded
                                            : CatalogDbErrorCategory::SqliteError;
            result.message = "live library change failed unexpectedly";
        }
        if (batch.catchUpRequired) {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            const bool transferredBarrierSucceeded = catchUpSucceeded && result.success;
            if (transferredBarrierSucceeded) {
                m_liveChangeDrainPendingCatchUp = false;
            } else if (!m_liveChangeStop) {
                // The source event batch has already been transferred and its
                // overflow bit was consumed.  Keep the catch-up barrier
                // queued until the whole transferred batch completes
                // successfully, including any item application after the
                // checkpoint advances.  Home only consumes the failure
                // publication and never owns retry policy.
                (void)m_liveChangeRequests.push(batch);
                m_liveChangeRequests.markCatchUpRequired();
                m_liveChangeDrainPendingCatchUp = true;
            }
        }
        result.generation = std::max(result.generation, committedGeneration);
        result.committedGeneration = std::max(result.committedGeneration, committedGeneration);
        publish(std::move(result));
    }
}

} // namespace library
} // namespace miyoofin
