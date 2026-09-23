#ifndef MIYOOFIN_HOME_SCREEN_HPP
#define MIYOOFIN_HOME_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../net/Session.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../download/DownloadManager.hpp"
#include "../../download/DownloadUi.hpp"
#include "../../download/DownloadHierarchy.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../library/LibraryQuery.hpp"
#include "../../library/LibraryCoordinator.hpp"
#include "../../net/JellyfinLibraryEvents.hpp"
#include <memory>
#include "../HomeSyncState.hpp"
#include "../HomeSettingsModel.hpp"
#include "../ArtworkLayout.hpp"
#include "../ShowsBrowser.hpp"
#include "../HomeTabs.hpp"
#include "../HomeArtworkPlan.hpp"
#include "HomeLibraryController.hpp"
#include "HomeArtworkController.hpp"
#include "../../diagnostics/TelemetryIds.hpp"
#include "../../update/UpdateManager.hpp"
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {

/// The main Jellyfin-style home screen with top tabs, horizontal
/// media rows, card grid, and info panel for the selected item.
/// Fetches real library data from the server on a background thread.
class HomeScreen : public Screen
{
  public:
    enum class ShowsFocusState
    {
        ShowsGrid,
        AnimeGrid,
        AlphabetRail
    };

    using SettingsRowAction = HomeSettingsRowAction;
    using SettingsAddressRow = HomeSettingsAddressRow;
    using PosterJob = HomePosterJob;
    explicit HomeScreen(const Session& session, std::shared_ptr<DownloadManager> downloads = {},
                        std::shared_ptr<library::LibraryQuery> libraryQuery = {},
                        std::shared_ptr<library::LibraryCoordinator> libraryCoordinator = {});
    ~HomeScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    bool handlePointerClick(int x, int y) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface* fb) override;
    const char* diagnosticName() const override
    {
        return "HomeScreen";
    }
    bool deferDestruction() const override
    {
        return true;
    }
    int diagnosticActiveTab() const
    {
        return m_activeTab;
    }
    const char* diagnosticTabName() const;

    static constexpr int kPosterThreads = HomeArtworkController::kPosterThreads;
    /// Maximum decode attempts for a cached JPEG before it receives a
    /// permanent RowArtworkStatus::Failed tombstone.  The per-key counter
    /// is reset on success and is naturally fresh when the key changes
    /// (new imageTag).  Defined here so tests can reference the bound.
    static constexpr int kMaxDecodeAttempts = HomeArtworkController::kMaxDecodeAttempts;
    static constexpr int settingsRowCount()
    {
        return homeSettingsBaseRowCount();
    }
    static SettingsRowAction settingsRowAction(int row);
    static std::vector<SettingsAddressRow> settingsAddressRows(const Session& session);
    static int settingsRowCount(const Session& session);
    static SettingsRowAction settingsRowAction(int row, const Session& session);
    static const char* lastApiRouteValue();

    /// True when the user has confirmed logout (App handles the transition).
    bool logoutRequested() const
    {
        return m_logoutRequested;
    }
    /// True when the user has confirmed changing servers (App handles the transition).
    bool changeServerRequested() const
    {
        return m_changeServerRequested;
    }
    bool takeLocalAddressRequest()
    {
        const bool requested = m_localAddressRequested;
        m_localAddressRequested = false;
        return requested;
    }
    bool takePublicAddressRequest()
    {
        const bool requested = m_publicAddressRequested;
        m_publicAddressRequested = false;
        return requested;
    }
    bool updateExitRequested() const
    {
        return m_updateExitRequested;
    }
    void setLocalServerUrl(const std::string& url)
    {
        m_session.localServerUrl = url;
    }
    void cancelAsyncWork() noexcept;
    void requestStopAllWorkers() noexcept;
    void joinAllWorkers();
    void setPublicServerUrl(const std::string& url)
    {
        m_session.publicServerUrl = url;
    }
    bool presentationOffline() const
    {
        return m_libraryOffline || m_session.manualOfflineMode;
    }

#ifdef MIYOOFIN_TEST_BUILD
    // Test-only deterministic seam: apply one immutable controller publication
    // exactly as the SDL lifecycle would, so the cold-provisional discard,
    // rail-only loading, and terminal publication rules can be verified
    // without a live fetch or SDL.  Not compiled into production builds.
    void applyFetchedPresentationForTest(const HomeLibraryController::Presentation& presentation);
    bool testLoadStateLoading() const
    {
        return m_loadState == LoadState::Loading;
    }
    bool testLoadStateReady() const
    {
        return m_loadState == LoadState::Ready;
    }
    bool testLoadStateError() const
    {
        return m_loadState == LoadState::Error;
    }
    bool testHaveCachedSnapshot() const
    {
        return m_haveCachedSnapshot;
    }
    const std::vector<TabData>& testTabs() const
    {
        return m_tabs;
    }
    const std::vector<MediaItem>& testMovieWindow() const
    {
        return m_movieWindow;
    }
#endif

    /// Replace, insert, or remove Home's Continue Watching row.
    /// Public so the row behaviour can be tested without a network request.
    static void updateContinueWatchingRow(std::vector<TabData>& tabs,
                                          const std::vector<MediaItem>& items);

    // --- Row artwork helpers (public for testing) -------------------------

    /// Build the row-artwork identity key for a media item.
    /// Format: "itemId:Primary:imageTag:WxH"
    static std::string rowArtworkKey(const MediaItem& item);
    static bool acceptsShowsArtworkResult(const HomeArtworkController::DecodeResult& result,
                                          std::uint64_t currentGeneration,
                                          const std::set<std::string>& activeKeys,
                                          const std::set<std::string>& protectedKeys);
    static std::vector<PosterJob> collectPosterJobs(const LibrarySnapshot& snapshot);
    /// Pure online Home projection; exposed to keep its cached row semantics testable.
    static std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot& snapshot);
    static std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot& snapshot);
    static library::MediaPage offlineMediaPage(const LibrarySnapshot& snapshot,
                                               const std::string& type, int alphabetLetter,
                                               std::size_t limit,
                                               const library::LibraryPageCursor& after = {});
    /// Offline contains only locally playable libraries; Home is intentionally absent.
    static std::vector<std::string> tabNames(const std::vector<TabData>& tabs);
    /// Keep a named tab across a layout change, falling back to Movies.
    static int transitionTabIndex(const std::vector<TabData>& from, int selected,
                                  const std::vector<TabData>& to);
    static int tabIndexAtPoint(const std::vector<TabData>& tabs, int x, int y);
    static ShowsFocusState showsFocusAfterRefresh(ShowsFocusState previous, bool hasShows,
                                                  bool hasAnime);
    static int restoreSelectionIndex(const std::vector<MediaItem>& items,
                                     const std::string& selectedId, int fallback);
    static int preserveGridScroll(int selected, int count, int currentScroll, int columns,
                                  int rows);
    /// Season posters use the exact dimensions of SeriesScreen's grid.
    static std::vector<PosterJob> collectSeasonPosterJobs(const std::vector<MediaItem>& seasons);
    /// Canonical artwork key for deduplication: "itemId:imageType:imageTag:WxH".
    static std::string posterJobKey(const PosterJob& job);
    /// Scheduling decision: should the poster worker process this job now?
    /// High-priority jobs always run; low-priority jobs are deferred while the
    /// initial population walk is still in progress.
    static bool shouldProcessPosterJob(bool highPriority, bool populationInProgress);

    /// Row artwork state map — public so tests can inspect it.
    std::map<std::string, RowArtworkEntry> m_rowArtwork;

    /// LRU eviction order: oldest key at front.  Loaded keys occur once.
    std::deque<std::string> m_rowArtworkOrder;

    // Pre-scaled card surface cache (performance: avoid per-frame create/scale/free)
    std::map<std::string, SDL_Surface*> m_cardSurfaceCache;

    // --- Offline snapshot cache (public for testing) -----------------------
    using OfflineSnapshotSignature = HomeLibraryController::OfflineSnapshotSignature;
    static OfflineSnapshotSignature computeOfflineSignature(const DownloadSnapshot& downloads,
                                                            std::uint64_t catalogGeneration = 0);

  private:
    enum class LoadState
    {
        Loading,
        Ready,
        Error
    };

    LoadState m_loadState = LoadState::Loading;

    // Tab / row / card navigation state
    int m_activeTab;
    int m_activeRow;
    int m_activeCard;
    int m_rowScroll;
    int m_cardScroll;
    int m_settingsSelected = 0;
    int m_settingsScroll = 0;

    // Tab data (owned, populated by background fetch)
    std::vector<TabData> m_tabs;
    // These are bounded windows populated by CatalogDb keyset pages.  They
    // are never the complete movie/show catalog.
    std::vector<MediaItem> m_movieWindow;
    int m_movieActiveLetter = -1;
    int m_movieAlphabetFocus = 0;
    bool m_movieRailFocused = false;
    enum class ShowsFocus
    {
        ShowsGrid,
        AnimeGrid,
        AlphabetRail
    };
    std::vector<MediaItem> m_showWindow, m_animeWindow;
    std::set<std::string> m_animeItemIds;
    std::vector<MediaItem> m_filteredShows, m_filteredAnime;
    ShowsFocus m_showsFocus = ShowsFocus::AlphabetRail;
    int m_showSelected = 0, m_animeSelected = 0, m_showScroll = 0, m_animeScroll = 0;
    int m_showsAlphabetFocus = 0, m_showsActiveLetter = -1;
    std::string m_moviePreviewId;
    std::string m_showsPreviewId;

    // Session info for API calls
    Session m_session;
    std::shared_ptr<DownloadManager> m_downloads;
    std::shared_ptr<library::LibraryQuery> m_libraryQuery;
    std::shared_ptr<library::LibraryCoordinator> m_libraryCoordinator;
    // Top-level catalog epoch: seeded from the persisted committed
    // generation and bumped at each point a top-level sync commits catalog
    // metadata (startup full sync, delta catch-up, safety catch-up and
    // reconcile, live-change catch-up/reconcile/apply — including a commit
    // whose later step fails).  Atomic so the SDL thread can read it for
    // the offline-snapshot signature without a DB query.  Hierarchy-only
    // commits (seasons/episodes) do NOT advance this epoch.
    std::string m_userName;

    struct MediaPageState
    {
        std::string type;
        int letter = -1;
        std::vector<MediaItem> items;
        library::LibraryPageCursor next;
        bool hasMore = true;
        bool hasEarlier = false;
        bool replaceWindowOnNextPage = false;
        bool pendingDown = false;
        bool inFlight = false;
        std::shared_ptr<std::atomic_bool> cancellation;
        std::future<library::MediaPage> future;
    };
    MediaPageState m_moviePage, m_showPage, m_animePage;

    // Logout (two-step confirm on Y)
    bool m_logoutArmed = false;
    Uint32 m_logoutTimer = 0;
    bool m_logoutRequested = false;
    enum class SettingsConfirmation
    {
        None,
        ChangeServer,
        Logout,
        CheckForUpdates
    };
    SettingsConfirmation m_settingsConfirmation = SettingsConfirmation::None;
    bool m_changeServerRequested = false;
    bool m_localAddressRequested = false;
    bool m_publicAddressRequested = false;

    // Update manager
    UpdateManager m_updateManager;
    UpdateSnapshot m_updateSnapshot;
    bool m_updateExitRequested = false;

    // Downloads is rendered only from this copied manager snapshot.  It is
    // refreshed in update(), never while rendering or handling input.
    DownloadSnapshot m_downloadSnapshot;
    int m_downloadSelected = 0, m_downloadScroll = 0;
    std::string m_downloadSelectedId, m_downloadConfirmId;
    std::vector<std::string> m_downloadConfirmItemIds;
    std::set<std::string> m_downloadExpanded;
    DownloadHierarchy m_downloadHierarchy;
    std::vector<OfflinePlaybackEntry> m_missingJournalEntries;
    std::string m_journalDiscardConfirmId;

    // Background fetch ownership lives in HomeLibraryController.  Home keeps
    // only the SDL-side application bookkeeping.
    std::unique_ptr<HomeLibraryController> m_libraryFetch;
    bool m_fetchPublished = false;
    bool m_fetchPostFinalizeApplied = false;
    bool m_fetchFailureRestored = false;
    std::string m_fetchError;
    LibrarySnapshot m_cachedSnapshot;
    LibrarySnapshot m_offlineSnapshot;
    LibrarySnapshot m_remoteSnapshot;
    std::vector<MediaItem> m_fetchRailCW;
    std::vector<MediaItem> m_fetchRailRA;
    bool m_fetchRailCWValid = false;
    bool m_fetchRailRAValid = false;
    bool m_haveCachedSnapshot = false;
    /// Set when the user toggles offline mode while a fetch is in-flight.
    /// finishFetch() re-fetches to synchronize the tabs with the new mode.
    bool m_offlineModeFetchPending = false;

    /// Try to instantly apply a cached offline snapshot when downloads
    /// haven't changed since the last offline build.  Returns true on
    /// success (tabs already applied); false if cache is stale.
    bool tryApplyCachedOfflineSnapshot();
    OfflineSnapshotSignature m_offlineSignature;
    bool m_haveOfflineSignature = false;
    LibrarySnapshot m_offlineSnapshotCache;
    bool m_haveOfflineSnapshotCache = false;
    LibrarySyncSchedule m_syncSchedule;
    bool m_libraryOffline = false;
    bool m_catalogScopeReadyLogged = false;
    bool m_firstMediaPageReadLogged = false;
    bool m_firstMediaPageReadCompletedLogged = false;
    bool m_firstUsefulHomeLogged = false;
    bool m_firstInteractiveFrameLogged = false;
    std::unique_ptr<HomeArtworkController> m_artworkController;
    // Session-level guard for bounded season-poster prefetch: each series is
    // prefetched at most once per process lifetime so repeat home fetches are
    // free.
    std::mutex m_hierarchyStateMutex;
    std::set<std::string> m_seasonPrefetchedIds;
    // The coordinator owns hierarchy work, cancellation, and generation.
    // Home retains only the request identity and UI-local progress state.
    std::atomic<std::uint64_t> m_hierarchyRequest{0};
    std::atomic<bool> m_hierarchyRequestReady{false};
    std::atomic<size_t> m_hierarchyCompleted{0}, m_hierarchyTotal{0};
    std::atomic<bool> m_hierarchyActive{false};
    std::atomic<bool> m_hierarchyOffline{false};
    bool m_hierarchySubmissionClosed = false;
    // Keys wanted by the current Shows viewport.  This is UI-thread state;
    // the controller uses it only to discard queued work from old working
    // sets; Home remains the authority for result freshness.
    std::set<std::string> m_activeShowsDecodeKeys;
    std::uint64_t m_showsArtworkGeneration = 0;

    /// Start a background library fetch.  Returns true if a new fetch was
    /// actually started; false if a previous fetch is still in-flight.
    using PendingPresentation = HomeLibraryController::Presentation;
    bool startFetch();
    void requestFetch(Uint32 now);
    void finishFetch();
    /// SDL-side application of one taken immutable publication.  Split out of
    /// finishFetch() so it can be exercised deterministically under
    /// MIYOOFIN_TEST_BUILD.
    void applyFetchedPresentation(const PendingPresentation& presentation);
    bool takePendingPresentation(PendingPresentation& presentation);
    void applyPendingPresentation(const PendingPresentation& presentation);
    void applyPresentationProjection();
    void restoreOnlinePresentation();
    void applyOfflineProjection();
    void prepareOfflineProjection();
    bool requestHierarchy(const std::vector<MediaItem>& shows, std::uint64_t generation,
                          bool forceReconcile);
    void consumeHierarchyResults();
    std::string syncStatusText() const;
    void drainDecodedArtwork();
    void submitDecode(const MediaItem& item, bool highPriority = false, bool shows = false);

    // DownloadManager may hold its mutex while reconciling/persisting on slow
    // SD storage.  Snapshot and playback-journal reads are therefore published
    // by this bounded worker only while the Downloads tab is visible.
    std::thread m_downloadRefreshThread;
    std::atomic<bool> m_downloadRefreshDone{false};
    std::atomic<bool> m_downloadRefreshInFlight{false};
    DownloadSnapshot m_downloadRefreshResult;
    std::vector<OfflinePlaybackEntry> m_downloadJournalResult;
    Uint32 m_downloadRefreshTimer = 0;
    void startDownloadRefresh();
    void finishDownloadRefresh();

    bool m_safetyReconcileInFlight = false;
    bool m_homeSyncActive = false;
    bool m_homeRailRefreshInFlight = false;
    bool m_homeRailRefreshPending = false;
    bool m_homeRailRefreshSucceeded = false;
    bool m_homeRailContinueValid = false;
    bool m_homeRailRecentValid = false;
    std::vector<MediaItem> m_homeRailContinueWatching;
    std::vector<MediaItem> m_homeRailRecentlyAdded;
    std::string m_homeRailRefreshError;
    std::int64_t m_lastHomeRailRefreshCompletedMs = 0;
    std::int64_t m_lastHomeRailRefreshAttemptMs = 0;

    /// Incremental rail publication during startup: set by the fetch worker
    /// after rails are fetched but before the full walk completes.
    bool m_homeRailsApplied = false;
    void updateLiveLibraryChanges();
    bool liveChangeAffectsHome(const library::LiveLibraryChangeResult& result) const;
    void publishLiveCatalogItems(const library::LiveLibraryChangeResult& result);
    void startHomeRailRefresh();
    void finishHomeRailRefresh();
    void finishSafetyReconcile();

    std::uint64_t catalogScopeEpoch() const;
    std::uint64_t committedCatalogGeneration() const;
    bool catalogScopeReady() const;

    // Helpers
    const TabData& currentTab() const;
    bool activeTabNamed(const char* name) const;
    int tabIndex(const char* name) const;
    const MediaRow* currentRow() const;
    const MediaItem* currentItem() const;

    void clampNavigation();
    /// Return the label of the currently focused Home row, or "" if none.
    std::string focusedHomeRowLabel() const;
    /// Reconcile m_activeRow by row label after a Home-row mutation.
    void restoreHomeRowFocus(const std::string& label);
    void activateTab(int index);
    void drawTabBar(SDL_Surface* fb);
    void drawInfoPanel(SDL_Surface* fb);
    void drawRowList(SDL_Surface* fb);
    void drawCard(SDL_Surface* fb, int x, int y, int w, int h, const MediaItem& item,
                  bool selected);
    void drawPlaceholderTab(SDL_Surface* fb, const char* message);
    void drawBottomHints(SDL_Surface* fb);
    void drawLoadingState(SDL_Surface* fb);
    void drawErrorState(SDL_Surface* fb);
    void refreshDownloads();
    bool handleDownloadsAction(Action action);
    void drawDownloadsTab(SDL_Surface* fb);
    void drawSettingsTab(SDL_Surface* fb);

    // Selected artwork state (B5b)
    DecodedImage m_selectedArtwork;
    std::string m_selectedArtworkId; // "itemId:imageTag" identity key
    bool m_selectedArtworkAttempted = false;

    void tryLoadSelectedArtwork();

    // Row artwork loading (B5d2a)
    void tryLoadOneRowArtwork();
    void evictRowArtworkIfNeeded();
    std::set<std::string> protectedRowArtworkKeys() const;
    void touchRowArtwork(const std::string& key);
    void storeDecodedRowArtwork(const std::string& key, DecodedImage image);
    void prepareCardSurface(const std::string& cacheKey, const DecodedImage& img, int boxW,
                            int boxH);
    void freeAllCardSurfaces();
    void updateShowsDecodeWorkingSet();
    void drawMovieGrid(SDL_Surface* fb);
    void drawMoviePreview(SDL_Surface* fb);
    void drawMovieAlphabetRail(SDL_Surface* fb);
    void refreshMovieFilter();
    void rebuildShowsPresentation();
    void refreshShowsFilter();
    void resetMediaPaging();
    void requestMediaPage(MediaPageState& state);
    void requestEarlierMediaPage(MediaPageState& state);
    void finishMediaPage(MediaPageState& state);
    void applyPendingDown(MediaPageState& state);
    void updateMediaPaging();
    const MediaItem* showsSelectedItem() const;
    void clampShowsNavigation();
    void drawShowsGrid(SDL_Surface* fb);
    void drawShowsPreview(SDL_Surface* fb);
    void drawShowsAlphabetRail(SDL_Surface* fb);
    int moveMovieGridCompact(int index, int count, int deltaRow, int deltaCol) const;
    static std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView>& views);
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SCREEN_HPP
