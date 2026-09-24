#include "LibraryCoordinator.hpp"
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

namespace {

constexpr bool kCoordinatorStartupSyncEnabled = true;

std::int64_t coordinatorWallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

bool coordinatorSupportedLibraryView(const miyoofin::LibraryView& view)
{
    return view.collectionType == "movies" || view.collectionType == "tvshows";
}

bool coordinatorLiveChangeIsEmpty(const miyoofin::JellyfinLibraryChangeBatch& batch)
{
    return !batch.catchUpRequired && !batch.userDataChanged && batch.itemsAdded.empty() &&
           batch.itemsUpdated.empty() && batch.itemsRemoved.empty();
}

void appendCoordinatorGateReason(std::string& reasons, const char* reason, bool blocked)
{
    if (!blocked)
        return;
    if (!reasons.empty())
        reasons += ',';
    reasons += reason;
}

bool coordinatorHierarchyIdentityMatches(const miyoofin::library::HierarchyRequest& left,
                                         const miyoofin::library::HierarchyRequest& right)
{
    if (left.kind != right.kind || left.generation != right.generation)
        return false;
    if (left.kind == miyoofin::library::HierarchyTaskKind::HomePrefetch)
        return false;
    if (left.series.id != right.series.id)
        return false;
    return left.kind != miyoofin::library::HierarchyTaskKind::SeasonEpisodes ||
           left.season.id == right.season.id;
}

} // namespace

namespace miyoofin {
namespace library {

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

bool LibraryCoordinator::takeFullPopulationUpdateLocked(std::uint64_t request,
                                                        FullPopulationUpdate& update)
{
    if (request == 0 || request != m_fullPopulationRequest || m_fullPopulationUpdates.empty())
        return false;
    update = std::move(m_fullPopulationUpdates.front());
    m_fullPopulationUpdates.pop_front();
    // The terminal publication retains the slot until Home consumes the whole
    // queue; draining it releases the serialized operation.  An intermediate
    // page that momentarily empties the queue does not, because the worker is
    // still executing.
    if (m_fullPopulationUpdates.empty() &&
        currentOperationKind() == OperationKind::FullPopulation &&
        currentOperationPhase() == OperationPhase::PublicationPending) {
        releaseOperation(OperationKind::FullPopulation);
        // Draining the terminal publication ends the cold-start sequence; a
        // live change retained during startup/population may now be applied.
        finishStartupSequenceLocked();
    }
    return true;
}

WaitStatus
LibraryCoordinator::waitFullPopulationUpdate(std::uint64_t request, FullPopulationUpdate& update,
                                             const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_startupMutex);
    for (;;) {
        if (takeFullPopulationUpdateLocked(request, update))
            return WaitStatus::Ready;
        if (request == 0 || request != m_fullPopulationRequest)
            return WaitStatus::InvalidRequest;
        if (m_stopped)
            return WaitStatus::Stopped;
        const bool populationExecuting = currentOperationKind() == OperationKind::FullPopulation &&
                                         currentOperationPhase() == OperationPhase::Executing;
        const bool cancelled =
            (consumerCancellation && consumerCancellation->load()) ||
            (m_fullPopulationCancellation && m_fullPopulationCancellation->load());
        if (cancelled) {
            // Intermediate pages remain observable.  The terminal publication
            // clears the serialized gate and is consumed before cancellation
            // is reported to a caller with no remaining result.
            if (populationExecuting) {
                m_startupWake.wait(lock);
                continue;
            }
            return WaitStatus::Cancelled;
        }
        if (!populationExecuting)
            return WaitStatus::Superseded;
        m_startupWake.wait(lock);
    }
}

bool LibraryCoordinator::takeFullPopulationUpdate(std::uint64_t request,
                                                  FullPopulationUpdate& update)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        took = takeFullPopulationUpdateLocked(request, update);
    }
    if (took)
        m_startupWake.notify_all();
    return took;
}

bool LibraryCoordinator::requestFullPopulation(std::uint64_t& request)
{
    std::thread priorThread;
    std::string admissionDiagnostic;
    bool ownsStartupSequence = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        AdmissionRequest admission;
        admission.requester = OperationKind::FullPopulation;
        admission.requireDb = true;
        admission.requireScope = true;
        std::string reasons;
        if (!serializedOperationAdmittedLocked(admission, &reasons)) {
            admissionDiagnostic =
                "[LibraryCoordinator] full_population_rejected phase=admission request=" +
                std::to_string(request) +
                " current_request=" + std::to_string(m_fullPopulationRequest) +
                " generation=" + std::to_string(m_fullPopulationGeneration) +
                " scope_epoch=" + std::to_string(m_scopeEpoch) + " reasons=" + reasons;
        } else {
            if (m_fullPopulationThread.joinable())
                priorThread = std::move(m_fullPopulationThread);
            beginOperation(OperationKind::FullPopulation);
            // Only a successful admission consumes the cold-start handoff or
            // reservation.  Consuming it up front would let an unrelated
            // operation already occupying the slot as the same FullPopulation
            // kind (for example a beginFullSync reservation) make this request
            // fail admission after it had stolen Home's handoff, clearing the
            // owner token and armed demand.  Deferring keeps the handoff,
            // token, and demand intact for the intended continuation.
            ownsStartupSequence = claimStartupSequenceLocked(OperationKind::FullPopulation);
            m_fullPopulationCancellation = std::make_shared<std::atomic_bool>(false);
            m_fullPopulationUpdates.clear();
            request = ++m_fullPopulationRequest;
            m_fullPopulationGeneration = 0;
        }
    }
    if (!admissionDiagnostic.empty()) {
        // The failed admission did not consume the handoff/reservation, so the
        // owner-held sequence and its armed demand remain intact.
        uiDiagnostics().log(admissionDiagnostic);
        return false;
    }

    std::string postJoinDiagnostic;
    try {
        // The prior-thread join is part of the admitted operation's unwind
        // path: if it throws, the serialized slot and cold-start demand must
        // still be released by the catch below.
        if (priorThread.joinable())
            priorThread.join();

        {
            std::lock_guard<std::mutex> lock(m_startupMutex);
            if (m_stopped || !m_running || !m_sync || !m_db) {
                std::string reasons;
                appendCoordinatorGateReason(reasons, "stopped", m_stopped);
                appendCoordinatorGateReason(reasons, "not_running", !m_running);
                appendCoordinatorGateReason(reasons, "missing_sync", !m_sync);
                appendCoordinatorGateReason(reasons, "missing_db", !m_db);
                postJoinDiagnostic =
                    "[LibraryCoordinator] full_population_rejected phase=post_join request=" +
                    std::to_string(request) +
                    " current_request=" + std::to_string(m_fullPopulationRequest) +
                    " generation=" + std::to_string(m_fullPopulationGeneration) +
                    " scope_epoch=" + std::to_string(m_scopeEpoch) + " reasons=" + reasons;
                releaseOperation(OperationKind::FullPopulation);
                if (ownsStartupSequence)
                    finishStartupSequenceLocked();
            } else {
                const auto sync = m_sync;
                const auto db = m_db;
                const Session session = m_session;
                const auto cancellation = m_fullPopulationCancellation;
                const std::uint64_t requestId = request;
                const std::uint64_t scopeEpoch = m_scopeEpoch;
                const std::string scopeHash =
                    (session.serverUrl.empty() || session.userId.empty())
                        ? "none"
                        : catalog::scopeKey(session.serverUrl, session.userId);
                m_fullPopulationThread = std::thread([this, sync, db, session, cancellation,
                                                      requestId, scopeEpoch, scopeHash] {
                    FullPopulationUpdate terminal;
                    terminal.request = requestId;
                    terminal.terminal = true;
                    std::uint64_t transactionGeneration = 0;
                    std::uint64_t committedGeneration = 0;
                    bool transactionStarted = false;
                    bool transactionCommitted = false;
                    std::vector<LibraryView> views;
                    std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
                    std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
                    std::size_t metadataTotal = 0;
                    std::size_t metadataCompleted = 0;
                    std::size_t mediaCount = 0;
                    std::size_t requestCount = 0;
                    std::size_t completedPageCount = 0;
                    bool firstPage = true;

                    const auto cancelled = [&] { return cancellation && cancellation->load(); };
                    const auto publish = [this, requestId, scopeEpoch,
                                          scopeHash](FullPopulationUpdate value) {
                        std::string diagnostic;
                        bool publicationMade = false;
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            // A request cannot normally be superseded while this worker is
                            // alive, but retain the identity check so a stale publication
                            // can never be consumed by a later Home fetch.
                            if (requestId != m_fullPopulationRequest) {
                                diagnostic =
                                    "[LibraryCoordinator] "
                                    "full_population_publish_rejected request=" +
                                    std::to_string(requestId) +
                                    " current_request=" + std::to_string(m_fullPopulationRequest) +
                                    " generation=" + std::to_string(value.generation) +
                                    " current_generation=" +
                                    std::to_string(m_fullPopulationGeneration) +
                                    " scope_epoch=" + std::to_string(scopeEpoch) +
                                    " scope_hash=" + scopeHash + " source=full_population" +
                                    " reason=request_mismatch";
                            } else {
                                value.request = requestId;
                                if (value.terminal)
                                    markOperationPublicationPending(OperationKind::FullPopulation);
                                m_fullPopulationUpdates.push_back(std::move(value));
                                publicationMade = true;
                                const auto& published = m_fullPopulationUpdates.back();
                                std::string kind =
                                    published.terminal
                                        ? (published.success
                                               ? "terminal_success"
                                               : (published.cancelled || published.superseded
                                                      ? "terminal_cancel"
                                                      : "terminal_failure"))
                                        : (published.firstPage ? "first_page" : "page");
                                diagnostic =
                                    "[LibraryCoordinator] full_population_publish request=" +
                                    std::to_string(published.request) +
                                    " generation=" + std::to_string(published.generation) +
                                    " scope_epoch=" + std::to_string(scopeEpoch) +
                                    " scope_hash=" + scopeHash + " source=full_population" +
                                    " kind=" + kind + " queue_depth=" +
                                    std::to_string(m_fullPopulationUpdates.size()) +
                                    " monotonic_us=" +
                                    std::to_string(TelemetryClock::monotonicUs());
                            }
                        }
                        if (publicationMade)
                            m_startupWake.notify_all();
                        uiDiagnostics().log(diagnostic);
                    };
                    const auto publishTerminal = [&](FullPopulationUpdate value) {
                        value.request = requestId;
                        value.generation =
                            transactionCommitted ? committedGeneration : m_catalogGeneration;
                        value.committedGeneration =
                            transactionCommitted ? committedGeneration : m_catalogGeneration;
                        value.terminal = true;
                        value.committed = transactionCommitted;
                        value.views = views;
                        value.moviesByView = moviesByView;
                        value.showsByView = showsByView;
                        value.metadataTotal = metadataTotal;
                        value.metadataCompleted = metadataCompleted;
                        value.mediaCount = mediaCount;
                        value.requestCount = requestCount;
                        publish(std::move(value));
                    };
                    const auto failForCancellation = [&] {
                        FullPopulationUpdate value;
                        value.cancelled = true;
                        value.error = CatalogDbErrorCategory::Superseded;
                        value.message = "full library population cancelled";
                        publishTerminal(std::move(value));
                    };

                    try {
                        if (!db || scopeEpoch == 0) {
                            terminal.error = CatalogDbErrorCategory::ScopeNotReady;
                            terminal.message = "CatalogDb scope is unavailable";
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        if (cancelled()) {
                            failForCancellation();
                            return;
                        }

                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            committedGeneration = m_catalogGeneration + 1;
                        }
                        transactionGeneration = sync->nextTransactionGeneration();
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            m_fullPopulationGeneration = transactionGeneration;
                        }
                        const auto begin = sync->begin(transactionGeneration).get();
                        if (!begin.success) {
                            terminal.error = begin.error;
                            terminal.message = begin.message;
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        transactionStarted = true;

                        std::string viewsError;
                        HttpClient client;
                        if (!RouteRequest(session).run(
                                [&](const std::string& base) {
                                    return JellyfinApi::getViews(
                                        base, session.accessToken, session.userId, session.deviceId,
                                        views, viewsError, client, cancellation.get());
                                },
                                viewsError)) {
                            // Mirror the page-fetch failure path: a request
                            // aborted by cancellation must publish a cancelled
                            // terminal, not a generic failure, so a consumer
                            // waiting on the serialized gate can distinguish
                            // the two.
                            terminal.cancelled = cancelled();
                            terminal.superseded =
                                !terminal.cancelled && cancellation && cancellation->load();
                            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                                : CatalogDbErrorCategory::None;
                            terminal.message =
                                viewsError.empty() ? "Failed to fetch libraries" : viewsError;
                            sync->abort(transactionGeneration).get();
                            transactionStarted = false;
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        views.erase(std::remove_if(views.begin(), views.end(),
                                                   [](const LibraryView& view) {
                                                       return !coordinatorSupportedLibraryView(
                                                           view);
                                                   }),
                                    views.end());

                        for (std::size_t viewOrdinal = 0; viewOrdinal < views.size();
                             ++viewOrdinal) {
                            const auto& view = views[viewOrdinal];
                            const std::string types =
                                view.collectionType == "tvshows" ? "Series" : "Movie";
                            int start = 0;
                            bool firstPageForView = true;
                            for (;;) {
                                if (cancelled()) {
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    failForCancellation();
                                    return;
                                }
                                LibraryItemsPage page;
                                std::string pageError;
                                ++requestCount;
                                if (!RouteRequest(session).run(
                                        [&](const std::string& base) {
                                            return JellyfinApi::getLibraryItemsPage(
                                                base, session.accessToken, session.userId,
                                                session.deviceId, view.id, types, start, 48, page,
                                                pageError, client, cancellation.get());
                                        },
                                        pageError)) {
                                    terminal.cancelled = cancelled();
                                    terminal.superseded =
                                        !terminal.cancelled && cancellation && cancellation->load();
                                    terminal.error = terminal.cancelled
                                                         ? CatalogDbErrorCategory::Superseded
                                                         : CatalogDbErrorCategory::None;
                                    terminal.message = pageError.empty()
                                                           ? "Failed to fetch library page"
                                                           : pageError;
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    publishTerminal(std::move(terminal));
                                    return;
                                }

                                if (firstPageForView) {
                                    metadataTotal +=
                                        page.totalRecordCount > 0
                                            ? static_cast<std::size_t>(page.totalRecordCount)
                                            : page.items.size();
                                    firstPageForView = false;
                                }
                                metadataCompleted += page.items.size();
                                mediaCount += page.items.size();

                                CatalogDbMediaPageWrite writePage;
                                writePage.items = page.items;
                                writePage.viewId = view.id;
                                writePage.viewName = view.name;
                                writePage.collectionType = view.collectionType;
                                writePage.ordinalStart = static_cast<std::size_t>(start);
                                writePage.viewOrdinal = static_cast<int>(viewOrdinal);
                                writePage.syncGeneration = transactionGeneration;
                                writePage.request = requestId;
                                writePage.diagnosticSource = "full_population";
                                writePage.finalPage = !page.hasMore;
                                CatalogDbMediaPageUpsertResult written;
                                try {
                                    written = sync->stage(writePage).get();
                                    if (written.success)
                                        ++completedPageCount;
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_complete"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " scope_epoch=" + std::to_string(scopeEpoch) +
                                        " scope_hash=" + scopeHash +
                                        " source=full_population page_kind=" +
                                        (writePage.collectionType == "movies"    ? "movies"
                                         : writePage.collectionType == "tvshows" ? "tvshows"
                                                                                 : "unknown") +
                                        " page_index=" + std::to_string(writePage.ordinalStart) +
                                        " status=" +
                                        (written.success ? "success"
                                                         : (written.cancelled || written.superseded
                                                                ? "cancelled"
                                                                : "failure")) +
                                        " error=" +
                                        std::to_string(static_cast<unsigned>(written.error)) +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                } catch (const std::exception&) {
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_exception"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " exception_class=std_exception" +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                    throw;
                                } catch (...) {
                                    uiDiagnostics().log(
                                        "[LibraryCoordinator] full_population_stage_exception"
                                        " request=" +
                                        std::to_string(requestId) +
                                        " generation=" + std::to_string(transactionGeneration) +
                                        " exception_class=unknown" +
                                        " completed_pages=" + std::to_string(completedPageCount) +
                                        " first_page=" + std::to_string(firstPage ? 1 : 0) +
                                        " monotonic_us=" +
                                        std::to_string(TelemetryClock::monotonicUs()));
                                    throw;
                                }
                                if (!written.success) {
                                    terminal.cancelled = written.cancelled;
                                    terminal.superseded = written.superseded;
                                    terminal.error = written.error;
                                    terminal.message = written.message.empty()
                                                           ? "Failed to persist library page"
                                                           : written.message;
                                    sync->abort(transactionGeneration).get();
                                    transactionStarted = false;
                                    publishTerminal(std::move(terminal));
                                    return;
                                }

                                auto& target =
                                    view.collectionType == "tvshows" ? showsByView : moviesByView;
                                if (target.empty() || target.back().first != view.name)
                                    target.emplace_back(view.name, std::vector<MediaItem>{});
                                auto& bounded = target.back().second;
                                if (bounded.size() < 24) {
                                    const std::size_t count = std::min<std::size_t>(
                                        24 - bounded.size(), page.items.size());
                                    bounded.insert(bounded.end(), page.items.begin(),
                                                   page.items.begin() +
                                                       static_cast<std::ptrdiff_t>(count));
                                }

                                FullPopulationUpdate update;
                                update.generation = committedGeneration;
                                update.pageValid = true;
                                update.firstPage = firstPage;
                                update.view = view;
                                update.page = page;
                                update.views = firstPage ? views : std::vector<LibraryView>{};
                                update.metadataTotal = metadataTotal;
                                update.metadataCompleted = metadataCompleted;
                                update.mediaCount = mediaCount;
                                update.requestCount = requestCount;
                                publish(std::move(update));
                                firstPage = false;

                                if (!page.hasMore || page.items.empty())
                                    break;
                                start = page.startIndex + static_cast<int>(page.items.size());
                            }
                        }

                        if (cancelled()) {
                            sync->abort(transactionGeneration).get();
                            transactionStarted = false;
                            failForCancellation();
                            return;
                        }
                        const auto finalized = sync->finalize(transactionGeneration).get();
                        transactionStarted = false;
                        if (!finalized.success) {
                            terminal.error = finalized.error;
                            terminal.message = finalized.message;
                            publishTerminal(std::move(terminal));
                            return;
                        }
                        transactionCommitted = true;
                        const std::int64_t nowMs = coordinatorWallClockMs();
                        {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            if (committedGeneration > m_catalogGeneration)
                                m_catalogGeneration = committedGeneration;
                        }
                        terminal.committed = true;
                        // Once finalize has committed the authoritative membership,
                        // the checkpoint is not cancellable: restart must not regress
                        // to the previous committed boundary.
                        const auto checkpoint =
                            sync->writeSyncState(nowMs, nowMs, committedGeneration).get();
                        terminal.checkpointCommitted = checkpoint.success;
                        terminal.checkpointMs = checkpoint.lastSuccessfulMs;
                        terminal.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                        terminal.lastReconcileMs = checkpoint.lastReconcileMs;
                        if (!checkpoint.success) {
                            terminal.error = checkpoint.error;
                            terminal.message = checkpoint.message.empty()
                                                   ? "Failed to write library sync checkpoint"
                                                   : checkpoint.message;
                        }
                        terminal.success = checkpoint.success;
                        if (checkpoint.success) {
                            std::lock_guard<std::mutex> guard(m_startupMutex);
                            m_lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                            m_lastReconcileMs = checkpoint.lastReconcileMs;
                        }
                        publishTerminal(std::move(terminal));
                    } catch (const std::exception& error) {
                        if (transactionStarted) {
                            try {
                                sync->abort(transactionGeneration).get();
                            } catch (...) {
                            }
                        }
                        FullPopulationUpdate failure;
                        failure.cancelled = cancelled();
                        failure.superseded = !failure.cancelled && transactionGeneration != 0;
                        failure.error = failure.cancelled ? CatalogDbErrorCategory::Superseded
                                                          : CatalogDbErrorCategory::SqliteError;
                        failure.message = error.what();
                        publishTerminal(std::move(failure));
                    } catch (...) {
                        if (transactionStarted) {
                            try {
                                sync->abort(transactionGeneration).get();
                            } catch (...) {
                            }
                        }
                        FullPopulationUpdate failure;
                        failure.cancelled = cancelled();
                        failure.superseded = !failure.cancelled && transactionGeneration != 0;
                        failure.error = failure.cancelled ? CatalogDbErrorCategory::Superseded
                                                          : CatalogDbErrorCategory::SqliteError;
                        failure.message = "full library population failed unexpectedly";
                        publishTerminal(std::move(failure));
                    }
                });
            }
        }
    } catch (...) {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        releaseOperation(OperationKind::FullPopulation);
        // Worker-thread construction or any other post-join failure must not
        // leave the cold-start demand armed, or retained live changes would be
        // deferred forever.  Only an admitted owner can reach this unwind path,
        // but gate on ownership so a non-owner can never clear the active
        // sequence's demand.
        if (ownsStartupSequence)
            finishStartupSequenceLocked();
        throw;
    }
    if (!postJoinDiagnostic.empty()) {
        uiDiagnostics().log(postJoinDiagnostic);
        return false;
    }
    return true;
}

void LibraryCoordinator::cancelFullPopulation() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        if (m_fullPopulationCancellation) {
            cancelled = true;
            m_fullPopulationCancellation->store(true);
        }
    }
    if (cancelled)
        m_startupWake.notify_all();
}

std::future<CatalogDbSyncState> LibraryCoordinator::checkpointLiveCatalog(
    std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs, std::uint64_t committedGeneration)
{
    return m_sync->writeSyncState(lastSuccessfulMs, lastReconcileMs, committedGeneration);
}

bool LibraryCoordinator::beginFullSync()
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    AdmissionRequest request;
    request.requester = OperationKind::FullPopulation;
    if (!serializedOperationAdmittedLocked(request, nullptr))
        return false;
    // Reserve the slot without a worker: callers use finishFullSync() to
    // release it.  No terminal publication is produced.
    beginOperation(OperationKind::FullPopulation);
    return true;
}

void LibraryCoordinator::finishFullSync() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    releaseOperation(OperationKind::FullPopulation);
}

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

bool LibraryCoordinator::requestHierarchy(const std::vector<MediaItem>& shows,
                                          std::uint64_t generation, bool forceReconcile,
                                          std::uint64_t& request)
{
    if (shows.empty() || generation == 0)
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;

    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8 || generation < m_hierarchyGeneration)
        return false;
    for (const auto& entry : m_hierarchyAccepted) {
        if (entry.second.kind == HierarchyTaskKind::HomePrefetch)
            return false;
    }
    request = ++m_hierarchyRequest;
    auto cancellation = std::make_shared<std::atomic_bool>(false);
    HierarchyRequest task{request, generation, forceReconcile, HierarchyTaskKind::HomePrefetch,
                          {},      {},         shows,          cancellation};
    m_hierarchyAccepted.emplace(request, task);
    m_hierarchyGeneration = generation;
    m_hierarchyCompleted = 0;
    m_hierarchyTotal = shows.size();
    m_hierarchyOffline = false;
    m_hierarchyForceReconcile = forceReconcile;
    m_hierarchyLastSuccessfulMs = 0;
    m_hierarchyLastReconcileMs = 0;
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::requestSeriesSeasons(const MediaItem& series, std::uint64_t& request)
{
    if (series.id.empty())
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8)
        return false;
    HierarchyRequest task;
    task.request = ++m_hierarchyRequest;
    task.generation = m_catalogGeneration;
    task.kind = HierarchyTaskKind::SeriesSeasons;
    task.series = series;
    task.cancellation = std::make_shared<std::atomic_bool>(false);
    for (const auto& entry : m_hierarchyAccepted) {
        if (coordinatorHierarchyIdentityMatches(entry.second, task))
            return false;
    }
    request = task.request;
    m_hierarchyAccepted.emplace(request, task);
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::requestSeasonEpisodes(const MediaItem& series, const MediaItem& season,
                                               std::uint64_t& request)
{
    if (series.id.empty() || season.id.empty())
        return false;
    std::lock_guard<std::mutex> startupLock(m_startupMutex);
    AdmissionRequest admission;
    admission.requester = OperationKind::Hierarchy;
    admission.requireQuery = true;
    if (!serializedOperationAdmittedLocked(admission, nullptr))
        return false;
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    if (m_hierarchyRequests.size() >= 8)
        return false;
    HierarchyRequest task;
    task.request = ++m_hierarchyRequest;
    task.generation = m_catalogGeneration;
    task.kind = HierarchyTaskKind::SeasonEpisodes;
    task.series = series;
    task.season = season;
    task.cancellation = std::make_shared<std::atomic_bool>(false);
    for (const auto& entry : m_hierarchyAccepted) {
        if (coordinatorHierarchyIdentityMatches(entry.second, task))
            return false;
    }
    request = task.request;
    m_hierarchyAccepted.emplace(request, task);
    beginOperation(OperationKind::Hierarchy);
    m_hierarchyRequests.push_back(std::move(task));
    m_hierarchyWake.notify_one();
    return true;
}

bool LibraryCoordinator::takeHierarchyResult(std::uint64_t request, HierarchyResult& result)
{
    bool took = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        took = takeHierarchyResultLocked(request, result);
    }
    if (took)
        m_hierarchyWake.notify_all();
    return took;
}

bool LibraryCoordinator::takeHierarchyResultLocked(std::uint64_t request, HierarchyResult& result)
{
    if (request == 0 || m_hierarchyResults.empty())
        return false;
    const auto found =
        std::find_if(m_hierarchyResults.begin(), m_hierarchyResults.end(),
                     [request](const HierarchyResult& value) { return value.request == request; });
    if (found == m_hierarchyResults.end())
        return false;
    result = std::move(*found);
    m_hierarchyResults.erase(found);
    if (result.terminal) {
        m_hierarchyAccepted.erase(request);
        m_hierarchySupersededRequests.insert(request);
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest &&
            m_hierarchyResults.empty()) {
            releaseOperation(OperationKind::Hierarchy);
        }
    }
    return true;
}

WaitStatus LibraryCoordinator::waitHierarchyResult(std::uint64_t request, HierarchyResult& result,
                                                   const std::atomic_bool* consumerCancellation)
{
    std::unique_lock<std::mutex> lock(m_hierarchyMutex);
    for (;;) {
        if (takeHierarchyResultLocked(request, result))
            return WaitStatus::Ready;
        if (request == 0)
            return WaitStatus::InvalidRequest;
        if (m_hierarchyStop)
            return WaitStatus::Stopped;
        if (consumerCancellation && consumerCancellation->load())
            return WaitStatus::Cancelled;
        const auto accepted = m_hierarchyAccepted.find(request);
        if (accepted == m_hierarchyAccepted.end())
            return m_hierarchySupersededRequests.count(request) ? WaitStatus::Superseded
                                                                : WaitStatus::InvalidRequest;
        if (accepted->second.cancellation && accepted->second.cancellation->load())
            return WaitStatus::Cancelled;
        m_hierarchyWake.wait(lock);
    }
}

void LibraryCoordinator::cancelHierarchyRequest(std::uint64_t request) noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        const auto accepted = m_hierarchyAccepted.find(request);
        if (accepted == m_hierarchyAccepted.end())
            return;
        accepted->second.cancellation->store(true);
        m_hierarchyRequests.erase(std::remove_if(m_hierarchyRequests.begin(),
                                                 m_hierarchyRequests.end(),
                                                 [request](const HierarchyRequest& value) {
                                                     return value.request == request;
                                                 }),
                                  m_hierarchyRequests.end());
        m_hierarchyResults.erase(std::remove_if(m_hierarchyResults.begin(),
                                                m_hierarchyResults.end(),
                                                [request](const HierarchyResult& value) {
                                                    return value.request == request;
                                                }),
                                 m_hierarchyResults.end());
        // The cancellation API is fire-and-forget.  Removing an active request
        // also suppresses its terminal publication once the network future
        // unwinds, so a caller that is leaving cannot strand scheduler state.
        m_hierarchySupersededRequests.insert(request);
        m_hierarchyAccepted.erase(accepted);
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest && m_hierarchyResults.empty())
            releaseOperation(OperationKind::Hierarchy);
        cancelled = true;
    }
    if (cancelled)
        m_hierarchyWake.notify_all();
}

void LibraryCoordinator::cancelHierarchy() noexcept
{
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        std::vector<std::uint64_t> homeRequests;
        for (const auto& entry : m_hierarchyAccepted) {
            if (entry.second.kind == HierarchyTaskKind::HomePrefetch)
                homeRequests.push_back(entry.first);
        }
        for (const auto request : homeRequests) {
            const auto accepted = m_hierarchyAccepted.find(request);
            if (accepted != m_hierarchyAccepted.end()) {
                accepted->second.cancellation->store(true);
                m_hierarchySupersededRequests.insert(request);
                m_hierarchyAccepted.erase(accepted);
                cancelled = true;
            }
        }
        // Cancellation belongs to the Home lifetime, not to the next Home
        // request.  Invalidate every publication from this lifetime and discard
        // anything Home did not consume before teardown.  A cancelled worker may
        // still be unwinding a network future; allowing the next request into the
        // queue keeps that worker serialized without letting its results reserve
        // the request slot forever.
        const auto oldRequestCount = m_hierarchyRequests.size();
        const auto oldResultCount = m_hierarchyResults.size();
        m_hierarchyRequests.erase(
            std::remove_if(m_hierarchyRequests.begin(), m_hierarchyRequests.end(),
                           [](const HierarchyRequest& value) {
                               return value.kind == HierarchyTaskKind::HomePrefetch;
                           }),
            m_hierarchyRequests.end());
        m_hierarchyResults.erase(
            std::remove_if(m_hierarchyResults.begin(), m_hierarchyResults.end(),
                           [](const HierarchyResult& value) {
                               return value.kind == HierarchyTaskKind::HomePrefetch;
                           }),
            m_hierarchyResults.end());
        cancelled = cancelled || oldRequestCount != m_hierarchyRequests.size() ||
                    oldResultCount != m_hierarchyResults.size();
        if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest && m_hierarchyResults.empty())
            releaseOperation(OperationKind::Hierarchy);
    }
    if (cancelled)
        m_hierarchyWake.notify_all();
}

void LibraryCoordinator::hierarchyWorker()
{
    for (;;) {
        HierarchyRequest request;
        std::shared_ptr<std::atomic_bool> cancellation;
        {
            std::unique_lock<std::mutex> lock(m_hierarchyMutex);
            m_hierarchyWake.wait(lock,
                                 [&] { return m_hierarchyStop || !m_hierarchyRequests.empty(); });
            if (m_hierarchyStop)
                return;
            request = std::move(m_hierarchyRequests.front());
            m_hierarchyRequests.pop_front();
            cancellation = request.cancellation;
            m_hierarchyActiveRequest = request;
            m_hierarchyActiveCancellation = cancellation;
        }

        const auto cancelled = [&] { return cancellation && cancellation->load(); };
        const auto loadSeasons = [&](const MediaItem& series) {
            HierarchyResult result;
            result.kind = request.kind;
            result.seriesId = series.id;
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season hierarchy request cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> seasons;
            const bool networkOk = RouteRequest(m_session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getSeasons(base, m_session.accessToken, m_session.userId,
                                                   m_session.deviceId, series.id, seasons, error,
                                                   cancellation.get());
                },
                error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season hierarchy request cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (m_db) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                std::map<std::string, std::vector<MediaItem>> episodesBySeason;
                for (const auto& season : seasons)
                    episodesBySeason.emplace(season.id, std::vector<MediaItem>{});
                const auto written =
                    m_db->stageSeriesHierarchy(series, seasons, episodesBySeason, 0,
                                               coordinatorWallClockMs(), false, metadata)
                        .get();
                if (written.cancelled || written.superseded || !written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    return result;
                }
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "season hierarchy request cancelled";
                return result;
            }
            result.success = true;
            result.seasons = std::move(seasons);
            return result;
        };
        const auto loadEpisodes = [&](const MediaItem& series, const MediaItem& season) {
            HierarchyResult result;
            result.kind = request.kind;
            result.seriesId = series.id;
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode hierarchy request cancelled";
                return result;
            }

            std::string error;
            std::vector<MediaItem> episodes;
            const bool networkOk = RouteRequest(m_session).run(
                [&](const std::string& base) {
                    return JellyfinApi::getEpisodes(base, m_session.accessToken, m_session.userId,
                                                    m_session.deviceId, series.id, season.id,
                                                    episodes, error, cancellation.get());
                },
                error);
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode hierarchy request cancelled";
                return result;
            }
            if (!networkOk) {
                result.message = error;
                return result;
            }

            if (m_db) {
                CatalogDbJobMetadata metadata;
                metadata.scopeEpoch = m_scopeEpoch;
                metadata.cancellation = cancellation;
                const auto written =
                    m_db->reconcileSeasonHierarchy(series, season, episodes, 0,
                                                   coordinatorWallClockMs(), metadata)
                        .get();
                if (written.cancelled || written.superseded || !written.success) {
                    result.cancelled = written.cancelled;
                    result.superseded = written.superseded;
                    result.error = written.error;
                    result.message = written.message;
                    return result;
                }
            }
            if (cancelled()) {
                result.cancelled = true;
                result.error = CatalogDbErrorCategory::Superseded;
                result.message = "episode hierarchy request cancelled";
                return result;
            }
            result.success = true;
            result.episodes = std::move(episodes);
            return result;
        };
        bool failed = false;
        std::size_t completed = 0;
        HierarchyResult terminal;
        terminal.request = request.request;
        terminal.generation = request.generation;
        terminal.kind = request.kind;
        terminal.terminal = true;

        const auto publish = [&](HierarchyResult result) {
            bool publicationMade = false;
            {
                std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                // A Home lifetime may discard its active request while the
                // network future unwinds.  A direct caller keeps its request
                // accepted until it consumes the terminal publication.
                if (m_hierarchyAccepted.find(request.request) == m_hierarchyAccepted.end())
                    return;
                result.request = request.request;
                result.generation = request.generation;
                result.kind = request.kind;
                m_hierarchyResults.push_back(std::move(result));
                publicationMade = true;
            }
            if (publicationMade)
                m_hierarchyWake.notify_all();
        };

        try {
            if (request.kind == HierarchyTaskKind::SeriesSeasons) {
                terminal = loadSeasons(request.series);
                terminal.terminal = true;
                publish(std::move(terminal));
            } else if (request.kind == HierarchyTaskKind::SeasonEpisodes) {
                terminal = loadEpisodes(request.series, request.season);
                terminal.terminal = true;
                publish(std::move(terminal));
            } else {
                for (const auto& series : request.shows) {
                    if (cancelled()) {
                        terminal.cancelled = true;
                        terminal.error = CatalogDbErrorCategory::Superseded;
                        terminal.message = "hierarchy refresh cancelled";
                        break;
                    }

                    HierarchyResult result;
                    result.kind = request.kind;
                    result.seriesId = series.id;
                    if (m_query) {
                        const auto cached = m_query->seasons(series.id, cancellation).get();
                        if (cached.success) {
                            result.cachedSeasons = std::move(cached.items);
                            if (!result.cachedSeasons.empty()) {
                                HierarchyResult cachedResult;
                                cachedResult.kind = request.kind;
                                cachedResult.seriesId = series.id;
                                cachedResult.cachedSeasons = result.cachedSeasons;
                                cachedResult.cacheOnly = true;
                                publish(std::move(cachedResult));
                                result.cachedSeasons.clear();
                            }
                        }
                    }

                    const auto seasonRefresh = loadSeasons(series);
                    if (!seasonRefresh.success) {
                        result.cancelled = seasonRefresh.cancelled;
                        result.superseded = seasonRefresh.superseded;
                        result.error = seasonRefresh.error;
                        result.message = seasonRefresh.message;
                        failed = true;
                        publish(std::move(result));
                        if (cancelled()) {
                            terminal.cancelled = true;
                            terminal.error = CatalogDbErrorCategory::Superseded;
                            terminal.message = "hierarchy refresh cancelled";
                            break;
                        }
                        continue;
                    }
                    result.seasons = seasonRefresh.seasons;

                    bool complete = true;
                    for (const auto& season : result.seasons) {
                        if (cancelled()) {
                            complete = false;
                            terminal.cancelled = true;
                            terminal.error = CatalogDbErrorCategory::Superseded;
                            terminal.message = "hierarchy refresh cancelled";
                            break;
                        }
                        if (season.id.empty()) {
                            complete = false;
                            break;
                        }
                        const auto episodeRefresh = loadEpisodes(series, season);
                        if (!episodeRefresh.success) {
                            complete = false;
                            result.cancelled = episodeRefresh.cancelled;
                            result.superseded = episodeRefresh.superseded;
                            result.error = episodeRefresh.error;
                            result.message = episodeRefresh.message;
                            break;
                        }
                    }

                    result.success = complete;
                    if (result.success) {
                        ++completed;
                        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                        if (m_hierarchyActiveRequest &&
                            m_hierarchyActiveRequest->request == request.request) {
                            ++m_hierarchyCompleted;
                        }
                    } else {
                        failed = true;
                    }
                    publish(std::move(result));
                    if (terminal.cancelled)
                        break;
                }
                if (cancelled()) {
                    terminal.cancelled = true;
                    terminal.error = CatalogDbErrorCategory::Superseded;
                    if (terminal.message.empty())
                        terminal.message = "hierarchy refresh cancelled";
                }
                if (!terminal.cancelled && !failed && completed == request.shows.size()) {
                    const std::int64_t nowMs = coordinatorWallClockMs();
                    const auto checkpoint =
                        m_sync
                            ->writeSyncState(nowMs, request.forceReconcile ? nowMs : 0,
                                             request.generation, cancellation)
                            .get();
                    terminal.checkpointCommitted = checkpoint.success;
                    terminal.checkpointMs = checkpoint.lastSuccessfulMs;
                    terminal.lastSuccessfulMs = checkpoint.lastSuccessfulMs;
                    terminal.lastReconcileMs = checkpoint.lastReconcileMs;
                    terminal.success = checkpoint.success;
                    terminal.error = checkpoint.error;
                    terminal.message = checkpoint.message;
                    if (checkpoint.success) {
                        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
                        if (m_hierarchyActiveRequest &&
                            m_hierarchyActiveRequest->request == request.request) {
                            m_hierarchyLastSuccessfulMs = checkpoint.lastSuccessfulMs;
                            m_hierarchyLastReconcileMs = checkpoint.lastReconcileMs;
                        }
                    }
                } else if (!terminal.cancelled && failed) {
                    terminal.message = "hierarchy refresh failed";
                }
                publish(std::move(terminal));
            }
        } catch (const std::exception& error) {
            terminal.request = request.request;
            terminal.generation = request.generation;
            terminal.kind = request.kind;
            terminal.terminal = true;
            terminal.cancelled = cancelled();
            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                : CatalogDbErrorCategory::SqliteError;
            terminal.message = error.what();
            publish(std::move(terminal));
        } catch (...) {
            terminal.request = request.request;
            terminal.generation = request.generation;
            terminal.kind = request.kind;
            terminal.terminal = true;
            terminal.cancelled = cancelled();
            terminal.error = terminal.cancelled ? CatalogDbErrorCategory::Superseded
                                                : CatalogDbErrorCategory::SqliteError;
            terminal.message = "hierarchy refresh failed unexpectedly";
            publish(std::move(terminal));
        }

        {
            std::lock_guard<std::mutex> lock(m_hierarchyMutex);
            if (m_hierarchyActiveRequest && m_hierarchyActiveRequest->request == request.request) {
                m_hierarchyActiveRequest.reset();
                m_hierarchyActiveCancellation.reset();
            }
            if (m_hierarchyRequests.empty() && !m_hierarchyActiveRequest &&
                m_hierarchyResults.empty()) {
                releaseOperation(OperationKind::Hierarchy);
            }
        }
    }
}

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

void LibraryCoordinator::cancelSafetyReconcile() noexcept
{
    std::lock_guard<std::mutex> lock(m_startupMutex);
    if (m_safetyReconcileCancellation)
        m_safetyReconcileCancellation->store(true);
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
