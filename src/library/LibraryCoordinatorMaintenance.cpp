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

bool LibraryCoordinator::requestSafetyReconcile(bool requireMaintenanceDue)
{
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (requireMaintenanceDue) {
            const auto nowMs = coordinatorWallClockMs();
            const bool due = m_lastReconcileMs <= 0 || nowMs < m_lastReconcileMs ||
                             nowMs - m_lastReconcileMs >= kSafetyReconcileIntervalMs;
            if (!due)
                return false;
        }
        AdmissionRequest admission;
        admission.requester = OperationKind::SafetyReconcile;
        admission.requireOnline = true;
        if (!serializedOperationAdmittedLocked(admission, nullptr))
            return false;
        if (m_safetyReconcileThread.joinable())
            priorThread = std::move(m_safetyReconcileThread);
        beginOperation(OperationKind::SafetyReconcile);
        m_safetyReconcileCancellation = std::make_shared<std::atomic_bool>(false);
        m_safetyReconcileResult = {};
        m_safetyReconcileResultReady = false;
    }
    if (priorThread.joinable())
        priorThread.join();

    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || !m_running || !m_sync) {
        releaseOperation(OperationKind::SafetyReconcile);
        return false;
    }

    const auto cancellation = m_safetyReconcileCancellation;
    const auto sync = m_sync;
    const auto db = m_db;
    const auto scopeEpoch = m_scopeEpoch;
    try {
        m_safetyReconcileThread = std::thread([this, cancellation, sync, db, scopeEpoch] {
            SafetyReconcileResult result;
            std::uint64_t committedGeneration = 0;
            std::uint64_t operationGeneration = 0;
            const auto cancelled = [&] { return cancellation && cancellation->load(); };
            const auto nextCommittedGeneration = [&] {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                return m_catalogGeneration + 1;
            };
            const auto commitGeneration = [&](std::uint64_t generation) {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                m_catalogGeneration = std::max(m_catalogGeneration, generation);
                committedGeneration = m_catalogGeneration;
                return committedGeneration;
            };
            const auto publish = [this](SafetyReconcileResult value) {
                std::lock_guard<std::mutex> guard(m_startupMutex);
                m_safetyReconcileResult = std::move(value);
                m_safetyReconcileResultReady = true;
                // The mutation is already durable, so release the global
                // serialized slot immediately and retain the immutable
                // terminal result for Home.  Same-kind admission is gated on
                // the result-ready flag above so a later reconcile cannot
                // overwrite this publication.
                releaseOperation(OperationKind::SafetyReconcile);
            };
            try {
                if (!db || scopeEpoch == 0) {
                    result.error = CatalogDbErrorCategory::ScopeNotReady;
                    result.message = "CatalogDb scope is unavailable";
                } else if (cancelled()) {
                    result.cancelled = true;
                    result.error = CatalogDbErrorCategory::Superseded;
                    result.message = "safety reconcile cancelled";
                } else {
                    CatalogDbJobMetadata metadata;
                    metadata.scopeEpoch = scopeEpoch;
                    metadata.cancellation = cancellation;
                    const auto state = db->readSyncState(false, 0, 0, metadata).get();
                    if (!state.success) {
                        result.error = state.error;
                        result.message = state.message;
                    } else {
                        sync->seedTransactionGeneration(state.committedGeneration);
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            if (state.committedGeneration > m_catalogGeneration)
                                m_catalogGeneration = state.committedGeneration;
                            m_lastSuccessfulMs = state.lastSuccessfulMs;
                            m_lastReconcileMs = state.lastReconcileMs;
                            result.lastSuccessfulMs = state.lastSuccessfulMs;
                            result.lastReconcileMs = state.lastReconcileMs;
                        }
                        if (cancelled()) {
                            result.cancelled = true;
                            result.error = CatalogDbErrorCategory::Superseded;
                            result.message = "safety reconcile cancelled";
                        } else {
                            operationGeneration = nextCommittedGeneration();
                            if (state.lastSuccessfulMs > 0) {
                                const auto catchUpGeneration = operationGeneration;
                                const auto catchUp =
                                    sync->catchUpChangedCatalog(state.lastSuccessfulMs,
                                                                cancellation, catchUpGeneration)
                                        .get();
                                if (!catchUp.success) {
                                    result.cancelled = catchUp.cancelled;
                                    result.superseded = catchUp.superseded;
                                    result.error = catchUp.error;
                                    result.message = catchUp.message;
                                    publish(std::move(result));
                                    return;
                                }
                                result.checkpointMs = catchUp.checkpointMs;
                                result.lastSuccessfulMs = catchUp.checkpointMs;
                                committedGeneration = commitGeneration(catchUpGeneration);
                                result.generation = committedGeneration;
                                result.committedGeneration = committedGeneration;
                                {
                                    std::lock_guard<std::mutex> guard(m_startupMutex);
                                    m_lastSuccessfulMs = result.lastSuccessfulMs;
                                }
                            }

                            if (cancelled()) {
                                result.cancelled = true;
                                result.error = CatalogDbErrorCategory::Superseded;
                                result.message = "safety reconcile cancelled";
                            } else {
                                committedGeneration = operationGeneration;
                                const auto transactionGeneration =
                                    sync->nextTransactionGeneration();
                                const auto reconciled = sync->reconcileAuthoritativeMembership(
                                                                cancellation, transactionGeneration,
                                                                committedGeneration)
                                                            .get();
                                if (!reconciled.success) {
                                    result.cancelled = reconciled.cancelled;
                                    result.superseded = reconciled.superseded;
                                    result.error = reconciled.error;
                                    result.message = reconciled.message;
                                } else {
                                    committedGeneration = commitGeneration(committedGeneration);
                                    result.generation = committedGeneration;
                                    result.committedGeneration = committedGeneration;
                                    const auto nowMs = coordinatorWallClockMs();
                                    // The authoritative commit is already
                                    // durable.  Finish its checkpoint without
                                    // cancellation so restart cannot regress
                                    // to the prior committed boundary.
                                    auto checkpoint =
                                        sync->writeSyncState(nowMs, nowMs, committedGeneration)
                                            .get();
                                    result.success = true;
                                    result.checkpointMs = checkpoint.success
                                                              ? checkpoint.lastSuccessfulMs
                                                              : result.checkpointMs;
                                    result.lastSuccessfulMs = checkpoint.success
                                                                  ? checkpoint.lastSuccessfulMs
                                                                  : result.lastSuccessfulMs;
                                    result.lastReconcileMs = checkpoint.success
                                                                 ? checkpoint.lastReconcileMs
                                                                 : result.lastReconcileMs;
                                    if (checkpoint.success) {
                                        std::lock_guard<std::mutex> guard(m_startupMutex);
                                        m_lastSuccessfulMs = result.lastSuccessfulMs;
                                        m_lastReconcileMs = result.lastReconcileMs;
                                    }
                                    if (!checkpoint.success && result.message.empty()) {
                                        result.error = checkpoint.error;
                                        result.message = checkpoint.message;
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (const std::exception& error) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = error.what();
            } catch (...) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = "safety reconcile failed unexpectedly";
            }
            result.generation = std::max(result.generation, committedGeneration);
            result.committedGeneration = std::max(result.committedGeneration, committedGeneration);
            publish(std::move(result));
        });
    } catch (...) {
        releaseOperation(OperationKind::SafetyReconcile);
        throw;
    }
    return true;
}

bool LibraryCoordinator::requestMaintenance()
{
    return requestSafetyReconcile(true);
}

#ifdef MIYOOFIN_TEST_BUILD
bool LibraryCoordinator::requestSafetyReconcileForTest()
{
    return requestSafetyReconcile(false);
}
#endif

bool LibraryCoordinator::takeSafetyReconcileResult(SafetyReconcileResult& result)
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (!m_safetyReconcileResultReady)
        return false;
    result = std::move(m_safetyReconcileResult);
    m_safetyReconcileResultReady = false;
    return true;
}

bool LibraryCoordinator::requestHomeRailRefresh(std::uint64_t& request)
{
    std::thread priorThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running || m_manualOfflineMode || m_homeRailInFlight ||
            m_homeRailResultReady)
            return false;
        if (m_homeRailThread.joinable())
            priorThread = std::move(m_homeRailThread);
        m_homeRailInFlight = true;
        m_homeRailCancellation = std::make_shared<std::atomic_bool>(false);
        request = ++m_homeRailRequest;
    }
    if (priorThread.joinable())
        priorThread.join();

    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped || !m_running) {
            m_homeRailInFlight = false;
            return false;
        }
        const auto cancellation = m_homeRailCancellation;
        const Session session = m_session;
        const std::uint64_t requestId = request;
        m_homeRailResult = {};
        m_homeRailResultReady = false;
        m_homeRailThread = std::thread([this, session, cancellation, requestId] {
            HomeRailResult result;
            result.request = requestId;
            HttpClient railClient;
            std::string continueError;
            std::string recentError;
            const bool continueOk = RouteRequest(session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getResumeItems(
                        base, session.accessToken, session.userId, session.deviceId, 12,
                        result.continueWatching, continueError, railClient, cancellation.get());
                },
                continueError);
            const bool recentOk = RouteRequest(session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getLatestItems(base, session.accessToken, session.userId,
                                                       session.deviceId, 16, result.recentlyAdded,
                                                       recentError, railClient, cancellation.get());
                },
                recentError);
            result.cancelled = cancellation && cancellation->load();
            result.continueValid = continueOk && !result.cancelled;
            result.recentlyAddedValid = recentOk && !result.cancelled;
            result.success = result.continueValid || result.recentlyAddedValid;
            if (!continueOk)
                result.error = continueError;
            else if (!recentOk)
                result.error = recentError;

            bool publicationMade = false;
            {
                std::lock_guard<std::mutex> lock(m_startupMutex);
                // A Home startup worker may be waiting on this publication
                // while teardown stops the coordinator.  Publish the
                // cancelled result even after stop; otherwise that worker's
                // wait loop has no completion signal and teardown deadlocks.
                if (requestId == m_homeRailRequest) {
                    m_homeRailResult = std::move(result);
                    m_homeRailResultReady = true;
                    publicationMade = true;
                }
                m_homeRailInFlight = false;
            }
            if (publicationMade)
                m_startupWake.notify_all();
        });
    }
    return true;
}

bool LibraryCoordinator::takeHomeRailResult(std::uint64_t request, HomeRailResult& result)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        took = takeHomeRailResultLocked(request, result);
    }
    if (took)
        m_startupWake.notify_all();
    return took;
}

bool LibraryCoordinator::takeHomeRailResultLocked(std::uint64_t request, HomeRailResult& result)
{
    if (!m_homeRailResultReady || m_homeRailInFlight || request == 0 ||
        m_homeRailResult.request != request)
        return false;
    result = std::move(m_homeRailResult);
    m_homeRailResultReady = false;
    return true;
}

WaitStatus LibraryCoordinator::waitHomeRailResult(std::uint64_t request, HomeRailResult& result,
                                                  const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_startupMutex);
    for (;;) {
        if (takeHomeRailResultLocked(request, result))
            return WaitStatus::Ready;
        if (request == 0 || request != m_homeRailRequest)
            return WaitStatus::InvalidRequest;
        if (m_stopped)
            return WaitStatus::Stopped;
        const bool cancelled = (consumerCancellation && consumerCancellation->load()) ||
                               (m_homeRailCancellation && m_homeRailCancellation->load());
        if (cancelled) {
            // Do not abandon the terminal publication: Home rail completion
            // owns the in-flight slot and must be consumed before reuse.
            if (m_homeRailInFlight) {
                m_startupWake.wait(lock);
                continue;
            }
            return WaitStatus::Cancelled;
        }
        if (!m_homeRailInFlight)
            return WaitStatus::Superseded;
        m_startupWake.wait(lock);
    }
}

void LibraryCoordinator::cancelHomeRailRefresh() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_homeRailCancellation) {
            cancelled = true;
            m_homeRailCancellation->store(true);
        }
    }
    if (cancelled)
        m_startupWake.notify_all();
}

void LibraryCoordinator::cancelSafetyReconcile() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_safetyReconcileCancellation)
        m_safetyReconcileCancellation->store(true);
}

} // namespace library
} // namespace miyoofin
