#ifndef MIYOOFIN_LIBRARY_COORDINATOR_HPP
#define MIYOOFIN_LIBRARY_COORDINATOR_HPP

#include "LibraryQuery.hpp"
#include "LibrarySync.hpp"
#include "../cache/LibraryCache.hpp"
#include "../net/JellyfinApi.hpp"
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {
namespace library {

/// Session-owned boundary for the shared online library services.
///
enum class StartupSyncMode { SkipFresh, DeltaCatchUp, FullReconcile };

struct StartupSyncResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    StartupSyncMode mode = StartupSyncMode::FullReconcile;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

/// Immutable incremental publication from the coordinator-owned full library
/// population.  Page data is published only after its CatalogDb stage has
/// committed, allowing Home to render the first bounded page while the worker
/// continues the remaining walk.
struct FullPopulationUpdate {
    std::uint64_t request = 0;
    std::uint64_t generation = 0;
    bool terminal = false;
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
struct SafetyReconcileResult {
    bool success = false;
    bool cancelled = false;
    bool superseded = false;
    CatalogDbErrorCategory error = CatalogDbErrorCategory::None;
    std::string message;
    std::uint64_t generation = 0;
    std::int64_t checkpointMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

/// Immutable publication from the coordinator-owned Home rail worker.  A
/// failed optional rail leaves its valid flag clear; consumers must retain
/// their previous value for that rail.
struct HomeRailResult {
    std::uint64_t request = 0;
    bool success = false;
    bool cancelled = false;
    bool continueValid = false;
    bool recentlyAddedValid = false;
    std::vector<MediaItem> continueWatching;
    std::vector<MediaItem> recentlyAdded;
    std::string error;
};

/// Coordinator publication for Home content and synchronization metadata.
/// Navigation, selection, scroll, and artwork state intentionally do not live
/// here: those remain owned by the SDL/HomeScreen thread.
struct HomeSyncStatus {
    bool inFlight = false;
    bool startupInFlight = false;
    bool fullSyncInFlight = false;
    bool safetyReconcileInFlight = false;
    bool cancelRequested = false;
    bool success = false;
    std::uint64_t generation = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
};

struct HomeState {
    std::uint64_t scopeEpoch = 0;
    std::uint64_t catalogGeneration = 0;
    // Assigned by LibraryCoordinator when a publication is accepted.
    std::uint64_t revision = 0;

    HomeSyncStatus sync;
    bool offline = false;
    bool stale = false;
    bool contentValid = false;
    bool cachedSnapshotValid = false;
    std::vector<TabData> tabs;
    LibrarySnapshot cachedSnapshot;

    bool continueValid = false;
    bool recentlyAddedValid = false;
    std::vector<MediaItem> continueWatching;
    std::vector<MediaItem> recentlyAdded;
    CatalogDbErrorCategory errorCategory = CatalogDbErrorCategory::None;
    std::string error;
};

struct LiveChangeIdentity {
    std::uint64_t worker = 0;
    std::uint64_t generation = 0;
    std::uint64_t request = 0;

    bool operator==(const LiveChangeIdentity &other) const
    {
        return worker == other.worker && generation == other.generation
            && request == other.request;
    }
};

/// The coordinator owns the lifecycle of LibrarySync and LibraryQuery and the
/// top-level startup sync policy.  LibrarySync remains the lower-level
/// CatalogDb/network primitive; this class is the one startup sync driver.
class LibraryCoordinator {
public:
    LibraryCoordinator(Session session, std::shared_ptr<CatalogDb> db,
                       std::uint64_t scopeEpoch);
    ~LibraryCoordinator();

    LibraryCoordinator(const LibraryCoordinator&) = delete;
    LibraryCoordinator& operator=(const LibraryCoordinator&) = delete;

    /// Start session-scoped live events. Safe to call more than once.
    void start();

    /// Start the coordinator-owned persisted-checkpoint decision and bounded
    /// startup catch-up.
    bool startStartupSync(bool catalogHasRows);
    bool takeStartupSyncResult(StartupSyncResult &result);
    /// Cancel and join the startup operation without stopping session scope.
    void cancelStartupSync() noexcept;

    /// Start one coordinator-owned full library population.  Incremental page
    /// publications become available through takeFullPopulationUpdate(); the
    /// terminal publication releases the serialized full-sync gate.
    bool requestFullPopulation(std::uint64_t &request);
    bool takeFullPopulationUpdate(std::uint64_t request,
                                  FullPopulationUpdate &update);
    void cancelFullPopulation() noexcept;

    /// Reserve/release the legacy full population slot.  Kept for callers
    /// which only need serialization without asking the coordinator to walk.
    bool beginFullSync();
    void finishFullSync() noexcept;

    /// Checkpoint a serialized live-catalog commit without exposing the
    /// LibrarySync transaction primitive to HomeScreen.
    std::future<CatalogDbSyncState> checkpointLiveCatalog(
        std::int64_t lastSuccessfulMs, std::int64_t lastReconcileMs,
        std::uint64_t committedGeneration);

    /// Start one serialized safety catch-up/reconcile.  The coordinator owns
    /// the checkpoint decision, worker, committed-generation policy, and
    /// cancellation lifetime.
    bool requestSafetyReconcile();
    bool takeSafetyReconcileResult(SafetyReconcileResult &result);
    void cancelSafetyReconcile() noexcept;

    /// Start one coordinator-owned Continue Watching/Recently Added refresh.
    /// The result is published atomically for the SDL thread to take later;
    /// a refresh never mutates HomeScreen state directly.
    bool requestHomeRailRefresh(std::uint64_t &request);
    bool takeHomeRailResult(std::uint64_t request, HomeRailResult &result);
    void cancelHomeRailRefresh() noexcept;

    /// Publish and consume immutable Home content/status snapshots.  A
    /// publication from another scope or an older catalog generation is
    /// rejected.  Missing content/rails retain the last valid cached value so
    /// transient failures cannot blank an already useful Home.
    bool publishHomeState(HomeState state);
    bool takeHomeState(std::shared_ptr<const HomeState> &state);

    /// Queue a live change for the next serialized consumer. Requests made
    /// during startup or full sync are retained rather than applied.
    bool requestLiveChange(const JellyfinLibraryChangeBatch &batch);
    bool takeLiveChangeRequest(JellyfinLibraryChangeBatch &batch,
                               LiveChangeIdentity &identity);
    bool publishLiveChangeResult(const LiveChangeIdentity &identity,
                                 LiveLibraryChangeResult result);
    bool takeLiveChangeResult(const LiveChangeIdentity &identity,
                              LiveLibraryChangeResult &result);
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

    std::shared_ptr<LibrarySync> sync() const { return m_sync; }
    std::shared_ptr<LibraryQuery> query() const { return m_query; }
    /// Refresh a series hierarchy through the coordinator-owned sync service.
    /// Callers retain ownership of the returned future and may cancel through
    /// the supplied token.
    std::future<HierarchyRefreshResult> refreshSeasons(
        const MediaItem &series,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    /// Refresh a season hierarchy through the coordinator-owned sync service.
    std::future<HierarchyRefreshResult> refreshEpisodes(
        const MediaItem &series, const MediaItem &season,
        const std::shared_ptr<std::atomic_bool> &cancellation = {});
    struct Status {
        bool inFlight = false;
        bool startupInFlight = false;
        bool fullSyncInFlight = false;
        bool safetyReconcileInFlight = false;
        bool cancelRequested = false;
        bool success = false;
        std::uint64_t generation = 0;
    };
    Status status() const;

private:
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
    std::thread m_safetyReconcileThread;
    std::shared_ptr<std::atomic_bool> m_safetyReconcileCancellation;
    SafetyReconcileResult m_safetyReconcileResult;
    bool m_safetyReconcileResultReady = false;
    bool m_safetyReconcileInFlight = false;
    std::uint64_t m_catalogGeneration = 0;
    JellyfinLibraryEventQueue m_liveChangeRequests;
    std::optional<LiveLibraryChangeResult> m_liveChangeResult;
    std::optional<LiveChangeIdentity> m_liveChangeActive;
    std::uint64_t m_liveChangeWorker = 0;
    std::uint64_t m_liveChangeRequest = 0;
    std::thread m_homeRailThread;
    std::shared_ptr<std::atomic_bool> m_homeRailCancellation;
    HomeRailResult m_homeRailResult;
    bool m_homeRailResultReady = false;
    bool m_homeRailInFlight = false;
    std::uint64_t m_homeRailRequest = 0;
    std::shared_ptr<const HomeState> m_homeState;
    std::shared_ptr<const HomeState> m_homeStateRetained;
    std::uint64_t m_homeStateRevision = 0;
    std::uint64_t m_homeStateCatalogGeneration = 0;
};

} // namespace library
} // namespace miyoofin

#endif // MIYOOFIN_LIBRARY_COORDINATOR_HPP
