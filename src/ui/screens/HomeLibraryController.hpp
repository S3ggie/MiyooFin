#ifndef MIYOOFIN_HOME_LIBRARY_CONTROLLER_HPP
#define MIYOOFIN_HOME_LIBRARY_CONTROLLER_HPP

#include "../../download/DownloadManager.hpp"
#include "../../library/LibraryCoordinator.hpp"
#include "../../library/LibraryQuery.hpp"
#include "../HomeArtworkPlan.hpp"
#include "../HomeTabs.hpp"
#include "../HomeSyncState.hpp"
#include "../../net/Session.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

/// Owns Home's blocking library work and publishes value-only results to the
/// SDL thread.  The service pointers are deliberately non-owning: Home keeps
/// the coordinator/query/download services alive through controller teardown.
class HomeLibraryController
{
  public:
    struct OfflineSnapshotSignature
    {
        std::size_t availableItemCount = 0;
        std::uint64_t totalDownloadedBytes = 0;
        std::uint64_t localBytes = 0;
        std::uint64_t reservedBytes = 0;
        std::uint64_t catalogGeneration = 0;
        std::set<std::string> availableItemIds;
        bool operator==(const OfflineSnapshotSignature& other) const
        {
            return availableItemCount == other.availableItemCount &&
                   totalDownloadedBytes == other.totalDownloadedBytes &&
                   localBytes == other.localBytes && reservedBytes == other.reservedBytes &&
                   catalogGeneration == other.catalogGeneration &&
                   availableItemIds == other.availableItemIds;
        }
        bool operator!=(const OfflineSnapshotSignature& other) const
        {
            return !(*this == other);
        }
    };

    struct ArtworkWork
    {
        std::vector<HomePosterJob> jobs;
        bool highPriority = false;
    };

    /// This is immutable once published through the pending slot.  It contains
    /// all worker output needed by Home to apply a presentation on the SDL
    /// thread, including rollback and hierarchy-submission side data.
    struct Presentation
    {
        std::vector<TabData> tabs;
        LibrarySnapshot cachedSnapshot;
        LibrarySnapshot remoteSnapshot;
        bool haveCachedSnapshot = false;
        bool contentValid = false;
        bool libraryOffline = false;
        bool cacheSaved = false;
        bool complete = false;
        bool catalogCommitted = false;
        bool cancelled = false;
        bool stale = false;
        bool railsReady = false;
        std::vector<MediaItem> continueWatching;
        std::vector<MediaItem> recentlyAdded;
        bool continueValid = false;
        bool recentlyAddedValid = false;
        bool offlineCacheValid = false;
        OfflineSnapshotSignature offlineSignature;
        LibrarySnapshot offlineSnapshotCache;
        bool offlinePrepared = false;
        std::vector<TabData> offlineTabs;
        LibrarySnapshot preparedOfflineSnapshot;
        std::set<std::string> animeItemIds;
        std::vector<ArtworkWork> artwork;
        bool hierarchyReady = false;
        std::vector<MediaItem> hierarchyShows;
        std::uint64_t hierarchyGeneration = 0;
        bool forceHierarchyReconcile = false;
        std::string error;
        std::uint64_t fetchGeneration = 0;

        // Presentation to restore if this fetch fails after a provisional
        // first-page publication.
        std::vector<TabData> previousTabs;
        LibrarySnapshot previousCachedSnapshot;
        LibrarySnapshot previousRemoteSnapshot;
        bool previousHaveCachedSnapshot = false;
        bool previousLibraryOffline = false;
        bool previousContentValid = false;
        std::set<std::string> previousAnimeItemIds;

        // Diagnostic-only handoff identity; these fields do not participate in
        // presentation selection or fetch completion.
        std::uint64_t diagnosticRequest = 0;
        std::uint64_t diagnosticGeneration = 0;
        std::size_t diagnosticCompletedPages = 0;
        std::string diagnosticStage;
    };

    struct RailPresentation
    {
        std::uint64_t request = 0;
        bool success = false;
        bool continueValid = false;
        bool recentlyAddedValid = false;
        std::vector<MediaItem> continueWatching;
        std::vector<MediaItem> recentlyAdded;
        std::string error;
    };

    HomeLibraryController(const Session& session, library::LibraryQuery* query,
                          library::LibraryCoordinator* coordinator, DownloadManager* downloads);
    ~HomeLibraryController();

    HomeLibraryController(const HomeLibraryController&) = delete;
    HomeLibraryController& operator=(const HomeLibraryController&) = delete;

    bool startFetch(const std::vector<TabData>& previousTabs,
                    const LibrarySnapshot& previousCachedSnapshot,
                    const LibrarySnapshot& previousRemoteSnapshot, bool previousHaveCachedSnapshot,
                    bool previousLibraryOffline, bool previousContentValid,
                    const std::set<std::string>& previousAnimeItemIds);
    bool takePresentation(Presentation& presentation);
    bool ready() const
    {
        return m_fetchReady.load();
    }
    bool done() const
    {
        return m_fetchDone.load();
    }
    bool complete() const
    {
        return m_fetchComplete.load();
    }
    bool cancelled() const
    {
        return m_fetchCancellation && m_fetchCancellation->load();
    }

    void setManualOfflineMode(bool offline)
    {
        m_session.manualOfflineMode = offline;
    }

    bool requestHomeRailRefresh();
    bool takeHomeRailRefresh(RailPresentation& result);

    void cancelFetch() noexcept;
    void requestStopAllWorkers() noexcept;
    void joinAllWorkers();

    bool initialPopulationInProgress() const
    {
        return m_initialPopulationInProgress.load();
    }
    bool metadataActive() const
    {
        return m_metadataActive.load();
    }
    std::size_t metadataCompleted() const
    {
        return m_metadataCompleted.load();
    }
    std::size_t metadataTotal() const
    {
        return m_metadataTotal.load();
    }
    bool artworkPlanningComplete() const
    {
        return m_artworkPlanningComplete.load();
    }
    std::uint64_t catalogScopeEpoch() const;
    std::uint64_t committedCatalogGeneration() const;

    static OfflineSnapshotSignature computeOfflineSignature(const DownloadSnapshot& downloads,
                                                            std::uint64_t catalogGeneration = 0);

  private:
    void publish(Presentation presentation);
    void fetchWorker(Session session, std::uint64_t fetchGeneration,
                     std::shared_ptr<std::atomic<bool>> cancellation);
    static void addArtwork(Presentation& presentation, std::vector<HomePosterJob> jobs,
                           bool highPriority);

    Session m_session;
    library::LibraryQuery* m_libraryQuery = nullptr;
    library::LibraryCoordinator* m_libraryCoordinator = nullptr;
    DownloadManager* m_downloads = nullptr;

    std::thread m_fetchThread;
    std::shared_ptr<std::atomic<bool>> m_fetchCancellation;
    std::atomic<bool> m_fetchDone{false};
    std::atomic<bool> m_fetchReady{false};
    std::atomic<bool> m_fetchComplete{false};
    std::atomic<std::uint64_t> m_fetchGeneration{0};
    std::mutex m_fetchMutex;
    std::shared_ptr<const Presentation> m_pendingPresentation;

    std::vector<TabData> m_previousTabs;
    LibrarySnapshot m_previousCachedSnapshot;
    LibrarySnapshot m_previousRemoteSnapshot;
    bool m_previousHaveCachedSnapshot = false;
    bool m_previousLibraryOffline = false;
    bool m_previousContentValid = false;
    std::set<std::string> m_previousAnimeItemIds;

    std::atomic<bool> m_fetchCatalogCommitted{false};
    std::atomic<std::size_t> m_metadataCompleted{0}, m_metadataTotal{0};
    std::atomic<bool> m_metadataActive{false};
    std::atomic<bool> m_artworkPlanningComplete{false};
    std::atomic<bool> m_initialPopulationInProgress{false};

    std::mutex m_railMutex;
    std::uint64_t m_homeRailRequest = 0;
    bool m_homeRailInFlight = false;

    std::atomic<bool> m_stopRequested{false};
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_LIBRARY_CONTROLLER_HPP
