#ifndef MIYOOFIN_LIBRARY_COORDINATOR_HPP
#define MIYOOFIN_LIBRARY_COORDINATOR_HPP

#include "LibraryQuery.hpp"
#include "LibraryChangeTypes.hpp"
#include "../catalog/CatalogDb.hpp"
#include "../net/JellyfinLibraryEvents.hpp"
#include "../net/Session.hpp"
#include "../net/JellyfinApi.hpp"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
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

    /// Start the coordinator-owned persisted-checkpoint decision and bounded
    /// startup catch-up.
    bool startStartupSync(bool catalogHasRows);
    bool takeStartupSyncResult(StartupSyncResult& result);
    /// Cancel and join the startup operation without stopping session scope.
    void cancelStartupSync() noexcept;

    /// Start one coordinator-owned full library population.  Incremental page
    /// publications become available through takeFullPopulationUpdate(); the
    /// terminal publication releases the serialized full-sync gate.
    bool requestFullPopulation(std::uint64_t& request);
    bool takeFullPopulationUpdate(std::uint64_t request, FullPopulationUpdate& update);
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
#endif

    std::shared_ptr<LibraryQuery> query() const
    {
        return m_query;
    }
    struct Status
    {
        bool inFlight = false;
        bool startupInFlight = false;
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
    std::thread m_startupThread;
    std::shared_ptr<std::atomic_bool> m_startupCancellation;
    StartupSyncResult m_startupResult;
    bool m_startupResultReady = false;
    bool m_startupInFlight = false;
    bool m_fullSyncInFlight = false;
    std::thread m_fullPopulationThread;
    std::shared_ptr<std::atomic_bool> m_fullPopulationCancellation;
    std::deque<FullPopulationUpdate> m_fullPopulationUpdates;
    std::uint64_t m_fullPopulationRequest = 0;
    std::uint64_t m_fullPopulationGeneration = 0;
    std::thread m_safetyReconcileThread;
    std::shared_ptr<std::atomic_bool> m_safetyReconcileCancellation;
    SafetyReconcileResult m_safetyReconcileResult;
    bool m_safetyReconcileResultReady = false;
    bool m_safetyReconcileInFlight = false;
    std::uint64_t m_catalogGeneration = 0;
    std::int64_t m_lastSuccessfulMs = 0;
    std::int64_t m_lastReconcileMs = 0;
    bool m_manualOfflineMode = false;
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
    std::optional<HierarchyRequest> m_hierarchyActiveRequest;
    std::shared_ptr<std::atomic_bool> m_hierarchyActiveCancellation;
    std::uint64_t m_hierarchyRequest = 0;
    std::uint64_t m_hierarchyGeneration = 0;
    std::size_t m_hierarchyCompleted = 0;
    std::size_t m_hierarchyTotal = 0;
    std::atomic_bool m_hierarchyMutationInFlight{false};
    bool m_hierarchyStop = false;
    bool m_hierarchyOffline = false;
    bool m_hierarchyForceReconcile = false;
    std::int64_t m_hierarchyLastSuccessfulMs = 0;
    std::int64_t m_hierarchyLastReconcileMs = 0;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_COORDINATOR_HPP
