#ifndef MIYOOFIN_LIBRARY_COORDINATOR_HPP
#define MIYOOFIN_LIBRARY_COORDINATOR_HPP

#include "LibraryQuery.hpp"
#include "LibraryChangeTypes.hpp"
#include "../catalog/CatalogDb.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/Session.hpp"
#include "../net/JellyfinApi.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {
namespace library {

class LibrarySync;

/// Session-owned boundary for the shared online library services.
///
enum class StartupSyncMode
{
    SkipFresh,
    DeltaCatchUp,
    FullReconcile
};

inline constexpr std::int64_t kSafetyReconcileIntervalMs = 24LL * 60 * 60 * 1000;

struct StartupSyncResult
{
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    StartupSyncMode mode = StartupSyncMode::FullReconcile;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::uint64_t committedGeneration = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

/// Outcome of a coordinator-owned blocking result wait.  Ready always means a
/// result was atomically consumed while holding the result domain mutex.
enum class WaitStatus
{
    Ready,
    Cancelled,
    Stopped,
    InvalidRequest,
    Superseded
};

/// Immutable incremental publication from the coordinator-owned full library
/// population.  Page data is published only after its CatalogDb stage has
/// committed, allowing Home to render the first bounded page while the worker
/// continues the remaining walk.
struct FullPopulationUpdate
{
    std::uint64_t request = 0;
    std::uint64_t generation = 0;
    std::uint64_t committedGeneration = 0;
    bool terminal = false;
    bool cacheOnly = false;
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    bool committed = false;
    bool checkpointCommitted = false;
    bool pageValid = false;
    bool firstPage = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::size_t metadataTotal = 0;
    std::size_t metadataCompleted = 0;
    std::size_t mediaCount = 0;
    std::size_t requestCount = 0;
    LibraryView view;
    LibraryItemsPage page;
    std::vector<LibraryView> views;
    std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
    std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
};

/// Terminal publication from the coordinator-owned periodic authoritative
/// reconcile.  `generation` is the last committed Home catalog epoch, not an
/// attempted generation: cancellation after a catch-up commit therefore
/// cannot make Home forget work that is already durable.
struct SafetyReconcileResult
{
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::uint64_t committedGeneration = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

/// Immutable publication from the coordinator-owned Home rail worker.  A
/// failed optional rail leaves its valid flag clear; consumers must retain
/// their previous value for that rail.
struct HomeRailResult
{
    std::uint64_t request = 0;
    bool success = false;
    bool cancelled = false;
    bool continueValid = false;
    bool recentlyAddedValid = false;
    std::vector<MediaItem> continueWatching;
    std::vector<MediaItem> recentlyAdded;
    std::string error;
};

enum class HierarchyTaskKind
{
    HomePrefetch,
    SeriesSeasons,
    SeasonEpisodes
};

/// One serialized hierarchy task.  The coordinator owns the cancellation
/// token after accepting the request; callers only retain the request id and
/// consume immutable results.
struct HierarchyRequest
{
    std::uint64_t request = 0;
    std::uint64_t generation = 0;
    bool forceReconcile = false;
    HierarchyTaskKind kind = HierarchyTaskKind::HomePrefetch;
    MediaItem series;
    MediaItem season;
    std::vector<MediaItem> shows;
    std::shared_ptr<std::atomic_bool> cancellation;
};

/// Per-series and terminal publications from the coordinator-owned hierarchy
/// worker.  A result may contain the cached seasons observed before the raw
/// refresh; this preserves cache-first artwork scheduling on Home.
struct HierarchyResult
{
    std::uint64_t request = 0;
    std::uint64_t generation = 0;
    std::uint64_t committedGeneration = 0;
    HierarchyTaskKind kind = HierarchyTaskKind::HomePrefetch;
    bool terminal = false;
    bool cacheOnly = false;
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    bool checkpointCommitted = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::string seriesId;
    std::vector<MediaItem> cachedSeasons;
    std::vector<MediaItem> seasons;
    std::vector<MediaItem> episodes;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

struct LiveChangeIdentity
{
    std::uint64_t worker = 0;
    std::uint64_t generation = 0;
    std::uint64_t request = 0;

    bool operator==(const LiveChangeIdentity& other) const
    {
        return worker == other.worker && generation == other.generation && request == other.request;
    }
};

/// The coordinator owns the lifecycle of LibrarySync and LibraryQuery and the
/// top-level startup sync policy.  LibrarySync remains the lower-level
/// CatalogDb/network primitive; this class is the one startup sync driver.
class LibraryCoordinator
{
  public:
    LibraryCoordinator(Session session, std::shared_ptr<CatalogDb> db, std::uint64_t scopeEpoch);
    ~LibraryCoordinator();

    LibraryCoordinator(const LibraryCoordinator&) = delete;
    LibraryCoordinator& operator=(const LibraryCoordinator&) = delete;

    /// Start session-scoped live events. Safe to call more than once.
    void start();

    /// Opaque owner identity for Home's cold-start sequence.  reserve returns
    /// one and only the matching value can release that reservation/handoff.
    /// kNoStartupSequenceToken is never a valid owner, so a controller that
    /// never reserved (or holds a superseded generation) cannot release.
    using StartupSequenceToken = std::uint64_t;
    static constexpr StartupSequenceToken kNoStartupSequenceToken = 0;

    /// Reserve cold-start precedence for Home's startup -> full-population
    /// sequence.  Call before start() (or before any live batch can be queued)
    /// so the live-change worker cannot claim the serialized slot ahead of
    /// startup.  Returns the owner token bound to this reservation generation;
    /// the owner-held reservation token is consumed by the first
    /// startup/full-population claim; the startup -> full-population handoff is
    /// tracked separately and consumed only by the intended FullPopulation
    /// continuation, so the sequence stays ownership-tracked until it
    /// completes, aborts, or its owner releases it.  Coordinators that never
    /// run Home's cold-start sequence (start-only live or hierarchy consumers)
    /// simply do not reserve and therefore process live work immediately.
    StartupSequenceToken reserveStartupSequence() noexcept;
    /// Relinquish the owner-held cold-start reservation when Home abandons its
    /// startup sequence.  `ownerToken` must match the token returned by the
    /// reserve call that armed this sequence; a stale token from a superseded
    /// reservation generation (or kNoStartupSequenceToken) is ignored so a
    /// non-owner destructor cannot clear another owner's handoff/demand.  This
    /// covers both a never-claimed reservation and the startup ->
    /// full-population handoff, where the demand remains armed while Home has
    /// not yet requested the full population.  It is a no-op while a
    /// startup/full-population operation actively owns the sequence, so it
    /// cannot clobber in-flight work.  Idempotent for the matching token.
    void releaseStartupSequence(StartupSequenceToken ownerToken) noexcept;

    /// Current cold-start sequence owner generation, or
    /// kNoStartupSequenceToken when no reservation/handoff is outstanding.
    /// Consumers that will later release the sequence (Home's controller)
    /// capture this at construction and pass it back to
    /// releaseStartupSequence(), binding the release to the generation that was
    /// armed when they were created rather than to whatever owner exists at
    /// teardown time.
    StartupSequenceToken startupSequenceOwnerToken() const noexcept;

    /// Start the coordinator-owned persisted-checkpoint decision and bounded
    /// startup catch-up.
    bool startStartupSync(bool catalogHasRows);
    bool takeStartupSyncResult(StartupSyncResult& result);
    WaitStatus waitStartupSyncResult(StartupSyncResult& result,
                                     const std::atomic_bool* consumerCancellation = nullptr);
    /// Cancel and join the startup operation without stopping session scope.
    void cancelStartupSync() noexcept;

    /// Start one coordinator-owned full library population.  Incremental page
    /// publications become available through takeFullPopulationUpdate(); the
    /// terminal publication releases the serialized full-sync gate.
    bool requestFullPopulation(std::uint64_t& request);
    bool takeFullPopulationUpdate(std::uint64_t request, FullPopulationUpdate& update);
    WaitStatus waitFullPopulationUpdate(std::uint64_t request, FullPopulationUpdate& update,
                                        const std::atomic_bool* consumerCancellation = nullptr);
    void cancelFullPopulation() noexcept;

    /// Reserve/release the legacy full population slot.  Kept for callers
    /// which only need serialization without asking the coordinator to walk.
    bool beginFullSync();
    void finishFullSync() noexcept;

    /// Checkpoint a serialized live-catalog commit without exposing the
    /// LibrarySync transaction primitive to HomeScreen.
    std::future<CatalogDbSyncState> checkpointLiveCatalog(std::int64_t lastSuccessfulMs,
                                                          std::int64_t lastReconcileMs,
                                                          std::uint64_t committedGeneration);

    /// Apply the coordinator-owned maintenance policy and, when due, start
    /// one serialized safety catch-up/reconcile.  The coordinator owns the
    /// checkpoint decision, worker, committed-generation policy, and
    /// cancellation lifetime.
    bool requestMaintenance();
#ifdef MIYOOFIN_TEST_BUILD
    // Test-only seam for exercising the lower-level worker and its
    // serialization behavior.  Production callers must use requestMaintenance
    // so they cannot bypass the maintenance policy.
    bool requestSafetyReconcileForTest();
#endif
    /// Keep the maintenance policy in step with the session's current offline
    /// mode.  The coordinator, not HomeScreen, decides whether maintenance is
    /// due or suppressed.
    void setManualOfflineMode(bool manualOffline) noexcept;
    bool takeSafetyReconcileResult(SafetyReconcileResult& result);
    void cancelSafetyReconcile() noexcept;

    /// Start one coordinator-owned Continue Watching/Recently Added refresh.
    /// The result is published atomically for the SDL thread to take later;
    /// a refresh never mutates HomeScreen state directly.
    bool requestHomeRailRefresh(std::uint64_t& request);
    bool takeHomeRailResult(std::uint64_t request, HomeRailResult& result);
    WaitStatus waitHomeRailResult(std::uint64_t request, HomeRailResult& result,
                                  const std::atomic_bool* consumerCancellation = nullptr);
    void cancelHomeRailRefresh() noexcept;

    /// Queue one serialized hierarchy walk for Home.  Results are published
    /// per series followed by one terminal checkpoint result.
    bool requestHierarchy(const std::vector<MediaItem>& shows, std::uint64_t generation,
                          bool forceReconcile, std::uint64_t& request);
    /// Queue one series-seasons or one season-episodes mutation through the
    /// same serialized hierarchy scheduler used by Home.  Results are
    /// immutable and are consumed with takeHierarchyResult().
    bool requestSeriesSeasons(const MediaItem& series, std::uint64_t& request);
    bool requestSeasonEpisodes(const MediaItem& series, const MediaItem& season,
                               std::uint64_t& request);
    bool takeHierarchyResult(std::uint64_t request, HierarchyResult& result);
    WaitStatus waitHierarchyResult(std::uint64_t request, HierarchyResult& result,
                                   const std::atomic_bool* consumerCancellation = nullptr);
    void cancelHierarchyRequest(std::uint64_t request) noexcept;
    /// Cancel the current Home lifetime, invalidate/discard its publications,
    /// and release the request slot for the next Home lifetime.  The worker
    /// itself remains serialized while a cancelled network future unwinds.
    void cancelHierarchy() noexcept;

    /// Queue a live change for the next serialized consumer. Requests made
    /// during startup or full sync are retained rather than applied. The
    /// coordinator-owned worker drains and applies the queued batch.
    bool requestLiveChange(const JellyfinLibraryChangeBatch& batch);
    bool takeLiveChangeResult(LiveLibraryChangeResult& result);
    void cancelLiveChange() noexcept;
    void discardLiveChangeResults() noexcept;

    /// Cancel and join coordinator-owned work. Safe to call more than once.
    void stop() noexcept;

    bool running() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_running;
    }
    bool stopped() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_stopped;
    }

#ifdef MIYOOFIN_TEST_BUILD
    // The application-facing coordinator boundary does not expose its raw
    // sync service.  Migration tests still need the service for direct
    // CatalogDb fixture setup, so keep that seam explicitly test-only.
    std::shared_ptr<LibrarySync> syncForTestSetup() const
    {
        return m_sync;
    }

    // Deterministic admission-snapshot race seam.  When armed, an admission
    // blocked by an occupied slot pauses after taking its operation snapshot;
    // a test can then release the slot in that window and prove the diagnostic
    // reason still comes from the snapshot.
    void setAdmissionSnapshotPauseForTest(bool pause) noexcept
    {
        m_admissionSnapshotPauseForTest.store(pause, std::memory_order_release);
    }
    bool admissionSnapshotPausedForTest() const noexcept
    {
        return m_admissionSnapshotPausedForTest.load(std::memory_order_acquire);
    }
    // Release whatever operation currently owns the serialized slot, without
    // going through a kind-specific public path.
    void releaseCurrentOperationForTest() noexcept
    {
        releaseOperation(currentOperationKind());
    }
    // True while a terminal result is retained for Home after the worker has
    // released the serialized slot.
    bool safetyReconcileResultReadyForTest() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_safetyReconcileResultReady;
    }
    bool liveChangeResultReadyForTest() const noexcept
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_liveChangeResult.has_value();
    }
    // Deterministic cold-start precedence seams.  waitLiveChangeDeferredForTest
    // blocks until the live-change worker has observed the cold-start demand
    // gate, proving it did not claim the serialized slot.
    // waitLiveChangeResultForTest blocks until a retained live result is
    // published once that demand clears.  Both return false only on timeout.
    bool waitLiveChangeDeferredForTest(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_startupMutex);
        return m_liveChangeTestWake.wait_for(
            lock, timeout, [this] { return m_liveChangeDemandDeferralsForTest > 0; });
    }
    bool waitLiveChangeResultForTest(std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(m_startupMutex);
        return m_liveChangeTestWake.wait_for(lock, timeout,
                                             [this] { return m_liveChangeResult.has_value(); });
    }
    // Deterministic cold-start ownership observation.  A failed competing
    // admission must leave the owning sequence's demand armed and claimed.
    bool startupPopulationDemandArmedForTest() const
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_startupPopulationDemand;
    }
    bool startupSequenceClaimedForTest() const
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_startupSequenceClaimed;
    }
    // True while the startup -> population handoff owner token is held by Home.
    // A competing startup admission in this window must not consume it.
    bool startupHandoffPendingForTest() const
    {
        std::lock_guard<std::mutex> lock(m_startupMutex);
        return m_startupHandoffPending;
    }
    // True when no serialized top-level operation currently owns the slot.  Used
    // to prove a competing startup is rejected by the handoff admission gate
    // rather than by slot occupancy.
    bool serializedOperationIdleForTest() const noexcept
    {
        return currentOperationKind() == OperationKind::None;
    }
#endif

    std::shared_ptr<LibraryQuery> query() const
    {
        return m_query;
    }
    struct Status
    {
        bool inFlight = false;
        bool startupInFlight = false;
        // True while a published startup result is awaiting consumption.
        bool startupResultReady = false;
        bool fullSyncInFlight = false;
        // Diagnostic-only publication identity/depth for bounded Home
        // consumer transition logging.
        std::uint64_t fullPopulationRequest = 0;
        std::uint64_t fullPopulationGeneration = 0;
        std::size_t fullPopulationQueueDepth = 0;
        bool safetyReconcileInFlight = false;
        bool cancelRequested = false;
        bool success = false;
        std::uint64_t generation = 0;
        std::uint64_t committedGeneration = 0;
        std::int64_t lastSuccessfulMs = 0;
        std::int64_t lastReconcileMs = 0;
        bool safetyReconcileDue = false;
        bool maintenanceDue = false;
        bool manualOffline = false;
    };
    Status status() const;

  private:
    // One serialized top-level operation owns the coordinator at a time.  Its
    // kind names the current holder; its phase distinguishes a worker still
    // executing from a terminal publication that retains the slot until Home
    // consumes it.  The encoded value is the single authority for the
    // admission gate, so every admission site asks the same helpers instead of
    // re-deriving a predicate from per-operation flags.
    enum class OperationKind : std::uint8_t
    {
        None = 0,
        Startup,
        FullPopulation,
        SafetyReconcile,
        LiveChange,
        Hierarchy
    };

    enum class OperationPhase : std::uint8_t
    {
        Idle = 0,
        Executing,
        PublicationPending
    };

    struct AdmissionRequest
    {
        OperationKind requester = OperationKind::None;
        bool requireDb = false;
        bool requireScope = false;
        bool requireQuery = false;
        bool requireOnline = false;
    };

    static constexpr std::uint32_t encodeOperation(OperationKind kind,
                                                   OperationPhase phase) noexcept
    {
        return (static_cast<std::uint32_t>(kind) << 8) | static_cast<std::uint32_t>(phase);
    }

    static OperationKind operationKindOf(std::uint32_t state) noexcept
    {
        return static_cast<OperationKind>(state >> 8);
    }

    static OperationPhase operationPhaseOf(std::uint32_t state) noexcept
    {
        return static_cast<OperationPhase>(state & 0xFFu);
    }

    OperationKind currentOperationKind() const noexcept;
    OperationPhase currentOperationPhase() const noexcept;
    void beginOperation(OperationKind kind) noexcept;
    void markOperationPublicationPending(OperationKind kind) noexcept;
    void releaseOperation(OperationKind kind) noexcept;
    const char* operationBlockReasonLocked(OperationKind kind, OperationPhase phase) const noexcept;
    // Single admission/accounting gate: lifecycle prerequisites, per-request
    // extras, and serialized-slot occupancy.  `reasons` receives the legacy
    // comma-separated diagnostic when it is non-null.
    bool serializedOperationAdmittedLocked(const AdmissionRequest& request,
                                           std::string* reasons) const noexcept;

    // Cold-start priority accounting.  While Home's initial startup/full
    // population sequence is expected or underway, the live-change worker must
    // not claim the serialized slot.  Both helpers require m_startupMutex.
    void markStartupPopulationDemandLocked() noexcept;
    void clearStartupPopulationDemandLocked() noexcept;
    // Finalizes cold-start ownership for a request whose admission has ALREADY
    // succeeded.  A startup/full-population request takes ownership of the
    // cold-start demand if no sequence already owns it.  After a startup ->
    // full-population handoff only the intended continuation (a FullPopulation
    // claim) consumes the handoff; a competing Startup claim must leave the
    // handoff owner and its demand untouched.  Returns true when this call is
    // the owner (and therefore responsible for clearing the demand on failure
    // or terminal completion); false for a competing request that must not
    // release the active owner's demand.  Callers MUST NOT invoke this before
    // admission succeeds: consuming the reservation/handoff up front would let
    // a failed admission (caused by another operation occupying the slot) clear
    // the owner-held sequence and its demand.  Requires m_startupMutex.
    bool claimStartupSequenceLocked(OperationKind requester) noexcept;
    // Ends the owning cold-start sequence: clears the demand gate and releases
    // ownership.  Requires m_startupMutex.
    void finishStartupSequenceLocked() noexcept;

    bool takeStartupSyncResultLocked(StartupSyncResult& result);
    bool takeFullPopulationUpdateLocked(std::uint64_t request, FullPopulationUpdate& update);
    bool takeHomeRailResultLocked(std::uint64_t request, HomeRailResult& result);
    bool takeHierarchyResultLocked(std::uint64_t request, HierarchyResult& result);
    bool requestSafetyReconcile(bool requireMaintenanceDue);
    void liveChangeWorker();
    void hierarchyWorker();

    Session m_session;
    std::shared_ptr<LibrarySync> m_sync;
    std::shared_ptr<LibraryQuery> m_query;
    std::shared_ptr<CatalogDb> m_db;
    std::uint64_t m_scopeEpoch = 0;
    bool m_running = false;
    bool m_stopped = false;

    mutable std::mutex m_startupMutex;
    std::condition_variable m_startupWake;
    // Serialized top-level slot authority.  Written under m_startupMutex for
    // Startup/FullPopulation/SafetyReconcile/LiveChange and under
    // m_hierarchyMutex for Hierarchy, so the encoded atomic keeps the
    // documented startup->hierarchy lock order.
    std::atomic<std::uint32_t> m_operationState{
        encodeOperation(OperationKind::None, OperationPhase::Idle)};
#ifdef MIYOOFIN_TEST_BUILD
    // Test-only admission-snapshot race seam state.
    std::atomic_bool m_admissionSnapshotPauseForTest{false};
    mutable std::atomic_bool m_admissionSnapshotPausedForTest{false};
    // Test-only cold-start precedence seam state.  Guarded by m_startupMutex.
    mutable std::condition_variable m_liveChangeTestWake;
    std::size_t m_liveChangeDemandDeferralsForTest = 0;
#endif
    std::thread m_startupThread;
    std::shared_ptr<std::atomic_bool> m_startupCancellation;
    StartupSyncResult m_startupResult;
    std::thread m_fullPopulationThread;
    std::shared_ptr<std::atomic_bool> m_fullPopulationCancellation;
    std::deque<FullPopulationUpdate> m_fullPopulationUpdates;
    std::uint64_t m_fullPopulationRequest = 0;
    std::uint64_t m_fullPopulationGeneration = 0;
    std::thread m_safetyReconcileThread;
    std::shared_ptr<std::atomic_bool> m_safetyReconcileCancellation;
    SafetyReconcileResult m_safetyReconcileResult;
    // True while a terminal safety result is retained for Home after the
    // worker has already released the serialized slot.  Same-kind admission is
    // gated on this so a later reconcile cannot overwrite the unconsumed
    // publication.
    bool m_safetyReconcileResultReady = false;
    std::uint64_t m_catalogGeneration = 0;
    std::int64_t m_lastSuccessfulMs = 0;
    std::int64_t m_lastReconcileMs = 0;
    bool m_manualOfflineMode = false;
    // Set while Home's cold-start startup/full-population sequence is expected
    // or in flight.  The live-change worker defers dequeue/claim while this is
    // set so background event processing cannot preempt startup or the initial
    // full population.  Guarded by m_startupMutex.
    bool m_startupPopulationDemand = false;
    // The owner-held cold-start reservation token: true between
    // reserveStartupSequence() and the first claim.  releaseStartupSequence()
    // may only clear the demand while this is set and no operation actively
    // claims the sequence, so an abandoned reservation can be released but
    // in-flight work cannot be clobbered.  Guarded by m_startupMutex.
    bool m_startupSequenceReservationOutstanding = false;
    // Owner identity of the current reservation generation.  reserve assigns a
    // fresh value (monotonic, never kNoStartupSequenceToken) and release only
    // acts when the caller passes the matching value, so a stale or non-owner
    // Home destructor cannot clear the intended owner's reservation/handoff.
    // It survives a claim and the startup -> population handoff (the owner
    // still holds the sequence) and is invalidated by finishStartupSequence()
    // or an owner release.  Guarded by m_startupMutex.
    StartupSequenceToken m_startupSequenceOwnerToken = kNoStartupSequenceToken;
    StartupSequenceToken m_nextStartupSequenceToken = kNoStartupSequenceToken;
    // True while a startup operation has handed the cold-start sequence off to
    // the full population Home is about to request.  Startup's worker ownership
    // has ended but Home still owns the armed demand, so the handoff is tracked
    // separately from an active claim: only a FullPopulation claim may consume
    // it, and releaseStartupSequence() may relinquish it.  Keeping it distinct
    // from m_startupSequenceClaimed means a competing startup/full admission
    // that loses the handoff cannot clear Home's demand when it fails.
    // Guarded by m_startupMutex.
    bool m_startupHandoffPending = false;
    // True while a claimed startup/full-population operation owns the demand.
    // Only the first claim takes ownership; a failed competing admission must
    // not clear another sequence's demand.  Guarded by m_startupMutex.
    bool m_startupSequenceClaimed = false;
    JellyfinLibraryEventQueue m_liveChangeRequests;
    std::optional<LiveLibraryChangeResult> m_liveChangeResult;
    std::optional<LiveChangeIdentity> m_liveChangeActive;
    std::uint64_t m_liveChangeWorker = 0;
    std::uint64_t m_liveChangeRequest = 0;
    std::thread m_liveChangeThread;
    std::shared_ptr<std::atomic_bool> m_liveChangeCancellation;
    std::condition_variable m_liveChangeWake;
    // A failed transfer from LibrarySync's bounded event queue has already
    // set the destination overflow bit, but keep this state until that
    // catch-up batch is consumed before draining the source again.
    bool m_liveChangeDrainPendingCatchUp = false;
    bool m_liveChangeStop = false;
    std::thread m_homeRailThread;
    std::shared_ptr<std::atomic_bool> m_homeRailCancellation;
    HomeRailResult m_homeRailResult;
    bool m_homeRailResultReady = false;
    bool m_homeRailInFlight = false;
    std::uint64_t m_homeRailRequest = 0;
    mutable std::mutex m_hierarchyMutex;
    std::condition_variable m_hierarchyWake;
    std::thread m_hierarchyThread;
    std::deque<HierarchyRequest> m_hierarchyRequests;
    std::deque<HierarchyResult> m_hierarchyResults;
    std::map<std::uint64_t, HierarchyRequest> m_hierarchyAccepted;
    std::set<std::uint64_t> m_hierarchySupersededRequests;
    std::optional<HierarchyRequest> m_hierarchyActiveRequest;
    std::shared_ptr<std::atomic_bool> m_hierarchyActiveCancellation;
    std::uint64_t m_hierarchyRequest = 0;
    std::uint64_t m_hierarchyGeneration = 0;
    std::size_t m_hierarchyCompleted = 0;
    std::size_t m_hierarchyTotal = 0;
    bool m_hierarchyStop = false;
    bool m_hierarchyOffline = false;
    bool m_hierarchyForceReconcile = false;
    std::int64_t m_hierarchyLastSuccessfulMs = 0;
    std::int64_t m_hierarchyLastReconcileMs = 0;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_COORDINATOR_HPP
