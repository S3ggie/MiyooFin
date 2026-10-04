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

LibraryCoordinator::OperationKind LibraryCoordinator::currentOperationKind() const noexcept
{
    return operationKindOf(m_operationState.load(std::memory_order_acquire));
}

LibraryCoordinator::OperationPhase LibraryCoordinator::currentOperationPhase() const noexcept
{
    return operationPhaseOf(m_operationState.load(std::memory_order_acquire));
}

void LibraryCoordinator::beginOperation(OperationKind kind) noexcept
{
    std::uint32_t expected = m_operationState.load(std::memory_order_acquire);
    for (;;) {
        const OperationKind current = operationKindOf(expected);
        if (current == kind)
            return;
        if (current != OperationKind::None)
            return;
        const std::uint32_t desired = encodeOperation(kind, OperationPhase::Executing);
        if (m_operationState.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
            return;
    }
}

void LibraryCoordinator::markOperationPublicationPending(OperationKind kind) noexcept
{
    std::uint32_t expected = m_operationState.load(std::memory_order_acquire);
    if (operationKindOf(expected) != kind)
        return;
    const std::uint32_t desired = encodeOperation(kind, OperationPhase::PublicationPending);
    m_operationState.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                             std::memory_order_acquire);
}

void LibraryCoordinator::releaseOperation(OperationKind kind) noexcept
{
    std::uint32_t expected = m_operationState.load(std::memory_order_acquire);
    if (operationKindOf(expected) != kind)
        return;
    const std::uint32_t desired = encodeOperation(OperationKind::None, OperationPhase::Idle);
    m_operationState.compare_exchange_strong(expected, desired, std::memory_order_acq_rel,
                                             std::memory_order_acquire);
}

const char* LibraryCoordinator::operationBlockReasonLocked(OperationKind kind,
                                                           OperationPhase phase) const noexcept
{
    // Derived from the caller's operation snapshot, not a fresh state read, so
    // a concurrent release cannot turn an occupied slot into a null reason.
    switch (kind) {
    case OperationKind::Startup:
        return phase == OperationPhase::PublicationPending ? "startup_result_ready"
                                                           : "startup_in_flight";
    case OperationKind::FullPopulation:
        return phase == OperationPhase::PublicationPending ? "pending_population_updates"
                                                           : "full_sync_in_flight";
    case OperationKind::SafetyReconcile:
        return phase == OperationPhase::PublicationPending ? "safety_reconcile_result_ready"
                                                           : "safety_reconcile_in_flight";
    case OperationKind::LiveChange:
        return "live_change_active";
    case OperationKind::Hierarchy:
        return "hierarchy_mutation_in_flight";
    case OperationKind::None:
    default:
        return nullptr;
    }
}

bool LibraryCoordinator::serializedOperationAdmittedLocked(const AdmissionRequest& request,
                                                           std::string* reasons) const noexcept
{
    const bool stopped = m_stopped;
    const bool notRunning = !m_running;
    const bool missingSync = !m_sync;
    const bool missingDb = request.requireDb && !m_db;
    const bool scopeNotReady = request.requireScope && m_scopeEpoch == 0;
    const bool missingQuery = request.requireQuery && !m_query;
    const bool offlineSuppressed = request.requireOnline && m_manualOfflineMode;
    // Snapshot kind and phase from a single load so the diagnostic reason is
    // always derived from the state that produced this rejection, even if the
    // slot is released concurrently.
    const std::uint32_t operationSnapshot = m_operationState.load(std::memory_order_acquire);
    const OperationKind current = operationKindOf(operationSnapshot);
    const OperationPhase currentPhase = operationPhaseOf(operationSnapshot);
    // Hierarchy is the one operation that may admit more requests while it
    // already owns the slot; every other requester must see it idle.
    const bool selfHierarchy =
        request.requester == OperationKind::Hierarchy && current == OperationKind::Hierarchy;
    const bool occupied = current != OperationKind::None && !selfHierarchy;
    // Safety and live workers release the serialized slot as soon as their
    // mutation is durable and retain the terminal result for Home.  A later
    // request of the same kind must not start (and overwrite the unconsumed
    // result); every other kind, including hierarchy, is admitted because the
    // slot is genuinely free.
    const bool safetyResultReady =
        request.requester == OperationKind::SafetyReconcile && m_safetyReconcileResultReady;
    const bool liveResultReady =
        request.requester == OperationKind::LiveChange && m_liveChangeResult.has_value();
    // While the startup -> full-population handoff is pending the serialized
    // slot is idle (the startup worker has released it, but Home still owns the
    // armed cold-start demand until it requests the intended population).  A
    // competing startup would otherwise pass the occupancy gate, be admitted,
    // and then clear Home's handoff/demand on its terminal path.  Reject it
    // explicitly; only requestFullPopulation (which consumes the handoff) may
    // proceed in this window.
    const bool startupHandoffPending =
        request.requester == OperationKind::Startup && m_startupHandoffPending;
    const bool blocked = stopped || notRunning || missingSync || missingDb || scopeNotReady ||
                         missingQuery || offlineSuppressed || occupied || safetyResultReady ||
                         liveResultReady || startupHandoffPending;
    if (!blocked)
        return true;
    if (reasons) {
#ifdef MIYOOFIN_TEST_BUILD
        // Deterministic race seam: hold the admission between its snapshot and
        // its diagnostic derivation so a test can release the occupied slot in
        // that window.
        if (occupied && m_admissionSnapshotPauseForTest.load(std::memory_order_acquire)) {
            m_admissionSnapshotPausedForTest.store(true, std::memory_order_release);
            while (m_admissionSnapshotPauseForTest.load(std::memory_order_acquire))
                std::this_thread::yield();
        }
#endif
        appendCoordinatorGateReason(*reasons, "stopped", stopped);
        appendCoordinatorGateReason(*reasons, "not_running", notRunning);
        appendCoordinatorGateReason(*reasons, "missing_sync", missingSync);
        appendCoordinatorGateReason(*reasons, "missing_db", missingDb);
        appendCoordinatorGateReason(*reasons, "scope_not_ready", scopeNotReady);
        appendCoordinatorGateReason(*reasons, "missing_query", missingQuery);
        appendCoordinatorGateReason(*reasons, "manual_offline", offlineSuppressed);
        if (occupied)
            appendCoordinatorGateReason(*reasons, operationBlockReasonLocked(current, currentPhase),
                                        true);
        if (safetyResultReady)
            appendCoordinatorGateReason(*reasons, "safety_reconcile_result_ready", true);
        if (liveResultReady)
            appendCoordinatorGateReason(*reasons, "live_change_result_ready", true);
        if (startupHandoffPending)
            appendCoordinatorGateReason(*reasons, "startup_handoff_pending", true);
    }
    return false;
}

void LibraryCoordinator::markStartupPopulationDemandLocked() noexcept
{
    // Manual offline never gates live processing: Home's startup/population
    // sequence is skipped offline and retained live changes must still apply.
    // The reservation/ownership flags are unaffected, so the demand is
    // re-armed when the session returns online.
    if (m_manualOfflineMode)
        return;
    m_startupPopulationDemand = true;
    // Wake the live worker so it re-evaluates the gate and defers even if it
    // was already runnable when the demand was marked.
    m_liveChangeWake.notify_all();
}

void LibraryCoordinator::clearStartupPopulationDemandLocked() noexcept
{
    m_startupPopulationDemand = false;
    // The live worker may be parked waiting for the cold-start sequence to
    // resolve; wake it so a retained batch is applied promptly.
    m_liveChangeWake.notify_all();
}

bool LibraryCoordinator::claimStartupSequenceLocked(OperationKind requester) noexcept
{
    bool ownsSequence = false;
    if (m_startupSequenceClaimed) {
        // An active startup/full-population operation already owns the demand.
        // A competing request must not take ownership: if its own admission
        // later fails it must not clear the owner's demand and release the
        // active startup gate.
        ownsSequence = false;
    } else if (m_startupHandoffPending) {
        // Startup handed off to the full population Home is about to request.
        // Only that intended continuation consumes the handoff owner; a
        // competing startup admission must not steal the token, or its failed
        // admission would clear Home's armed demand before the population runs.
        if (requester == OperationKind::FullPopulation) {
            ownsSequence = true;
            m_startupHandoffPending = false;
            m_startupSequenceClaimed = true;
        }
    } else {
        // Unclaimed base reservation (or a coordinator that never reserved):
        // the first startup/full-population request owns the sequence and
        // consumes the owner-held reservation token.
        ownsSequence = true;
        m_startupSequenceClaimed = true;
        m_startupSequenceReservationOutstanding = false;
    }
    markStartupPopulationDemandLocked();
    return ownsSequence;
}

void LibraryCoordinator::finishStartupSequenceLocked() noexcept
{
    // The sequence is terminal (completed, failed, cancelled, or superseded):
    // release the active claim, the owner-held reservation, and the startup ->
    // population handoff so a later release cannot resurrect a completed
    // sequence and re-armed offline sessions start from a clean slate.  The
    // owner token is invalidated so a delayed owner/non-owner release of the
    // completed generation cannot match a later reservation.
    m_startupSequenceClaimed = false;
    m_startupSequenceReservationOutstanding = false;
    m_startupHandoffPending = false;
    m_startupSequenceOwnerToken = kNoStartupSequenceToken;
    clearStartupPopulationDemandLocked();
}

LibraryCoordinator::LibraryCoordinator(Session session, std::shared_ptr<CatalogDb> db,
                                       std::uint64_t scopeEpoch)
    : m_session(session), m_sync(std::make_shared<LibrarySync>(std::move(session), db, scopeEpoch)),
      m_query(std::shared_ptr<LibraryQuery>(new LibraryQuery(db, scopeEpoch))), m_db(std::move(db)),
      m_scopeEpoch(scopeEpoch)
{
    m_manualOfflineMode = m_session.manualOfflineMode;
}

LibraryCoordinator::~LibraryCoordinator()
{
    stop();
}

void LibraryCoordinator::start()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_stopped || m_running || !m_sync)
        return;
    m_sync->startLiveEvents();
    {
        std::lock_guard<std::mutex> hierarchyLock(m_hierarchyMutex);
        m_hierarchyStop = false;
    }
    // The cold-start reservation is owned explicitly by the startup sequence,
    // not armed for every online start.  An owner that will run Home's startup
    // calls reserveStartupSequence() before start() so the live worker cannot
    // claim the slot ahead of startup; a start-only coordinator leaves the
    // reservation clear and processes live work immediately.
    m_liveChangeStop = false;
    m_liveChangeThread = std::thread(&LibraryCoordinator::liveChangeWorker, this);
    m_hierarchyThread = std::thread(&LibraryCoordinator::hierarchyWorker, this);
    m_running = true;
}

LibraryCoordinator::StartupSequenceToken
LibraryCoordinator::startupSequenceOwnerToken() const noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    return m_startupSequenceOwnerToken;
}

LibraryCoordinator::StartupSequenceToken LibraryCoordinator::reserveStartupSequence() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    // A fresh reservation is a new owner generation: assign a token that no
    // earlier (now stale) controller can present, then arm the reservation and
    // the demand.  An unclaimed handoff from the previous generation stays
    // armed, but the new token supersedes it for release purposes.
    m_startupSequenceOwnerToken = ++m_nextStartupSequenceToken;
    m_startupSequenceReservationOutstanding = true;
    markStartupPopulationDemandLocked();
    return m_startupSequenceOwnerToken;
}

void LibraryCoordinator::releaseStartupSequence(StartupSequenceToken ownerToken) noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    // Only the current owner generation may release.  A stale token from an
    // earlier reservation, or kNoStartupSequenceToken from a controller that
    // never reserved, must not clear the active owner's handoff/demand.
    if (ownerToken == kNoStartupSequenceToken || ownerToken != m_startupSequenceOwnerToken)
        return;
    // Only an unclaimed reservation or an unclaimed startup -> population
    // handoff is releasable.  Once a startup or full population request has
    // actively claimed the sequence, the coordinator owns the demand and
    // clears it on the sequence's terminal publication; the owner must not
    // clobber that in-flight work.
    if (m_startupSequenceClaimed)
        return;
    if (!m_startupSequenceReservationOutstanding && !m_startupHandoffPending)
        return;
    m_startupSequenceReservationOutstanding = false;
    m_startupHandoffPending = false;
    m_startupSequenceOwnerToken = kNoStartupSequenceToken;
    clearStartupPopulationDemandLocked();
}

void LibraryCoordinator::setManualOfflineMode(bool manualOffline) noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    const bool wasManualOffline = m_manualOfflineMode;
    m_manualOfflineMode = manualOffline;
    if (manualOffline) {
        // Home's startup/full-population sequence is skipped offline, so
        // cold-start precedence must not strand retained live changes.  An
        // in-flight startup or population still defers the live worker through
        // slot occupancy and clears its demand when it terminates.
        clearStartupPopulationDemandLocked();
    } else if (wasManualOffline &&
               (m_startupSequenceReservationOutstanding || m_startupHandoffPending)) {
        // Returning online with the cold-start reservation or the startup ->
        // population handoff still unclaimed: re-arm the demand before Home can
        // start its online fetch so a retained live change cannot claim the
        // serialized slot ahead of the sequence.  (mark is a no-op while
        // offline, hence the ordering above.)
        markStartupPopulationDemandLocked();
    }
}

void LibraryCoordinator::stop() noexcept
{
    std::thread startupThread;
    std::thread fullPopulationThread;
    std::thread homeRailThread;
    std::thread safetyReconcileThread;
    std::thread liveChangeThread;
    std::thread hierarchyThread;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_stopped)
            return;
        m_running = false;
        m_stopped = true;
        if (m_startupCancellation)
            m_startupCancellation->store(true);
        if (m_fullPopulationCancellation)
            m_fullPopulationCancellation->store(true);
        if (m_homeRailCancellation)
            m_homeRailCancellation->store(true);
        if (m_safetyReconcileCancellation)
            m_safetyReconcileCancellation->store(true);
        if (m_liveChangeCancellation)
            m_liveChangeCancellation->store(true);
        m_liveChangeStop = true;
        if (m_startupThread.joinable())
            startupThread = std::move(m_startupThread);
        if (m_fullPopulationThread.joinable())
            fullPopulationThread = std::move(m_fullPopulationThread);
        if (m_homeRailThread.joinable())
            homeRailThread = std::move(m_homeRailThread);
        if (m_safetyReconcileThread.joinable())
            safetyReconcileThread = std::move(m_safetyReconcileThread);
        if (m_liveChangeThread.joinable())
            liveChangeThread = std::move(m_liveChangeThread);
    }
    m_startupWake.notify_all();
    m_liveChangeWake.notify_all();
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        m_hierarchyStop = true;
        if (m_hierarchyActiveCancellation)
            m_hierarchyActiveCancellation->store(true);
        for (auto& request : m_hierarchyRequests) {
            if (request.cancellation)
                request.cancellation->store(true);
            HierarchyResult result;
            result.request = request.request;
            result.generation = request.generation;
            result.kind = request.kind;
            result.terminal = true;
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "hierarchy request cancelled by coordinator stop";
            m_hierarchyResults.push_back(std::move(result));
        }
        m_hierarchyRequests.clear();
        hierarchyThread = std::move(m_hierarchyThread);
    }
    m_hierarchyWake.notify_all();
    // The startup worker publishes through m_startupMutex, so never join it
    // while holding that mutex.
    if (startupThread.joinable())
        startupThread.join();
    if (fullPopulationThread.joinable())
        fullPopulationThread.join();
    if (homeRailThread.joinable())
        homeRailThread.join();
    if (safetyReconcileThread.joinable())
        safetyReconcileThread.join();
    if (liveChangeThread.joinable())
        liveChangeThread.join();
    if (hierarchyThread.joinable())
        hierarchyThread.join();
    if (m_sync)
        m_sync->stop();
}

LibraryCoordinator::Status LibraryCoordinator::status() const
{
    Status status;
    if (m_sync) {
        const auto syncStatus = m_sync->status();
        status.inFlight = syncStatus.inFlight;
        status.success = syncStatus.success;
        status.generation = syncStatus.generation;
    }
    std::lock_guard<std::mutex> lock(m_startupMutex);
    const OperationKind operation = currentOperationKind();
    const OperationPhase phase = currentOperationPhase();
    status.startupInFlight =
        operation == OperationKind::Startup && phase == OperationPhase::Executing;
    status.startupResultReady =
        operation == OperationKind::Startup && phase == OperationPhase::PublicationPending;
    status.fullSyncInFlight =
        operation == OperationKind::FullPopulation && phase == OperationPhase::Executing;
    status.fullPopulationRequest = m_fullPopulationRequest;
    status.fullPopulationGeneration = m_fullPopulationGeneration;
    status.fullPopulationQueueDepth = m_fullPopulationUpdates.size();
    status.safetyReconcileInFlight =
        operation == OperationKind::SafetyReconcile && phase == OperationPhase::Executing;
    status.committedGeneration = m_catalogGeneration;
    status.lastSuccessfulMs = m_lastSuccessfulMs;
    status.lastReconcileMs = m_lastReconcileMs;
    status.manualOffline = m_manualOfflineMode;
    const auto nowMs = coordinatorWallClockMs();
    status.safetyReconcileDue = status.lastReconcileMs <= 0 || nowMs < status.lastReconcileMs ||
                                nowMs - status.lastReconcileMs >= kSafetyReconcileIntervalMs;
    status.maintenanceDue = status.safetyReconcileDue && !status.manualOffline;
    status.cancelRequested = m_startupCancellation && m_startupCancellation->load();
    status.inFlight = status.inFlight || status.startupInFlight || status.fullSyncInFlight ||
                      status.safetyReconcileInFlight;
    return status;
}

} // namespace library
} // namespace miyoofin
