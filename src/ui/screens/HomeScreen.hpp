#ifndef MIYOOFIN_HOME_SCREEN_HPP
#define MIYOOFIN_HOME_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../data/MediaItem.hpp"
#include "../../net/Session.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../cache/SyncState.hpp"
#include "../../download/DownloadManager.hpp"
#include "../../download/DownloadUi.hpp"
#include "../../download/DownloadHierarchy.hpp"
#include "../../playback/OfflinePlaybackJournal.hpp"
#include "../../catalog/CatalogDb.hpp"
#include <memory>
#include "../HomeSyncState.hpp"
#include "../HomeSettingsModel.hpp"
#include "../ArtworkLayout.hpp"
#include "../ShowsBrowser.hpp"
#include "../HomeTabs.hpp"
#include "../HomeArtworkPlan.hpp"
#include "../../diagnostics/TelemetryIds.hpp"
#include <atomic>
#include <algorithm>
#include <condition_variable>
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
class HomeScreen : public Screen {
public:
    using SettingsRowAction = HomeSettingsRowAction;
    using SettingsAddressRow = HomeSettingsAddressRow;
    using PosterJob = HomePosterJob;
    explicit HomeScreen(const Session &session,
                        std::shared_ptr<DownloadManager> downloads={},
                        std::shared_ptr<CatalogDb> catalogDb={},
                        std::uint64_t catalogScopeEpoch=0);
    ~HomeScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;
    const char *diagnosticName() const override { return "HomeScreen"; }
    bool deferDestruction() const override { return true; }
    int diagnosticActiveTab() const { return m_activeTab; }
    const char *diagnosticTabName() const;

    static constexpr int settingsRowCount() { return homeSettingsBaseRowCount(); }
    static SettingsRowAction settingsRowAction(int row);
    static std::vector<SettingsAddressRow> settingsAddressRows(const Session &session);
    static int settingsRowCount(const Session &session);
    static SettingsRowAction settingsRowAction(int row, const Session &session);
    static const char *lastApiRouteValue();

    /// Submit the same complete subtree used by the online hierarchy worker.
    /// This narrow seam keeps the worker-to-CatalogDb boundary regression-testable.
    std::future<CatalogDbHierarchyWriteResult> submitCatalogHierarchyForTest(
        const MediaItem &series, const std::vector<MediaItem> &seasons,
        const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
        std::uint64_t generation, bool complete);

    /// True when the user has confirmed logout (App handles the transition).
    bool logoutRequested() const { return m_logoutRequested; }
    /// True when the user has confirmed changing servers (App handles the transition).
    bool changeServerRequested() const { return m_changeServerRequested; }
    bool takeLocalAddressRequest() { const bool requested = m_localAddressRequested; m_localAddressRequested = false; return requested; }
    bool takePublicAddressRequest() { const bool requested = m_publicAddressRequested; m_publicAddressRequested = false; return requested; }
    void setLocalServerUrl(const std::string &url) { m_session.localServerUrl = url; }
    void cancelAsyncWork() noexcept;
    void setPublicServerUrl(const std::string &url) { m_session.publicServerUrl = url; }
    bool presentationOffline() const { return m_libraryOffline || m_session.manualOfflineMode; }

    /// Replace, insert, or remove Home's Continue Watching row.
    /// Public so the row behaviour can be tested without a network request.
    static void updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items);

    // --- Row artwork helpers (public for testing) -------------------------

    /// Build the row-artwork identity key for a media item.
    /// Format: "itemId:Primary:imageTag:WxH"
    static std::string rowArtworkKey(const MediaItem &item);
    static std::vector<PosterJob> collectPosterJobs(const LibrarySnapshot &snapshot);
    /// Pure online Home projection; exposed to keep its cached row semantics testable.
    static std::vector<TabData> tabsFromSnapshot(const LibrarySnapshot &snapshot);
    static std::vector<TabData> offlineTabsFromSnapshot(const LibrarySnapshot &snapshot);
    /// Offline contains only locally playable libraries; Home is intentionally absent.
    static std::vector<std::string> tabNames(const std::vector<TabData> &tabs);
    /// Keep a named tab across a layout change, falling back to Movies.
    static int transitionTabIndex(const std::vector<TabData> &from, int selected,
                                  const std::vector<TabData> &to);
    /// Season posters use the exact dimensions of SeriesScreen's grid.
    static std::vector<PosterJob> collectSeasonPosterJobs(const std::vector<MediaItem> &seasons);

    /// Row artwork state map — public so tests can inspect it.
    std::map<std::string, RowArtworkEntry> m_rowArtwork;

    /// LRU eviction order: oldest key at front.  Loaded keys occur once.
    std::deque<std::string> m_rowArtworkOrder;

private:
    enum class LoadState { Loading, Ready, Error };

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
    enum class ShowsFocus { ShowsGrid, AnimeGrid, AlphabetRail };
    std::vector<MediaItem> m_showWindow, m_animeWindow;
    std::vector<MediaItem> m_filteredShows, m_filteredAnime;
    ShowsFocus m_showsFocus = ShowsFocus::AlphabetRail;
    int m_showSelected=0, m_animeSelected=0, m_showScroll=0, m_animeScroll=0;
    int m_showsAlphabetFocus=0, m_showsActiveLetter=-1;
    std::string m_showsPreviewId;

    // Session info for API calls
    Session m_session;
    std::shared_ptr<DownloadManager> m_downloads;
    std::shared_ptr<CatalogDb> m_catalogDb;
    CatalogDbJobMetadata m_catalogMetadata;
    std::string m_userName;

    struct MediaPageState {
        std::string type;
        int letter = -1;
        std::vector<MediaItem> items;
        CatalogDbPageCursor next;
        bool hasMore = true;
        bool inFlight = false;
        std::shared_ptr<std::atomic_bool> cancellation;
        std::future<CatalogDbMediaPageResult> future;
    };
    MediaPageState m_moviePage, m_showPage;

    // Logout (two-step confirm on Y)
    bool m_logoutArmed = false;
    Uint32 m_logoutTimer = 0;
    bool m_logoutRequested = false;
    enum class SettingsConfirmation { None, ChangeServer, Logout };
    SettingsConfirmation m_settingsConfirmation = SettingsConfirmation::None;
    bool m_changeServerRequested = false;
    bool m_localAddressRequested = false;
    bool m_publicAddressRequested = false;

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

    // Background fetch
    std::thread m_fetchThread;
    std::shared_ptr<std::atomic<bool>> m_fetchCancellation;
    std::atomic<bool> m_fetchDone{false};
    std::string m_fetchError;
    std::vector<TabData> m_fetchResult;
    LibrarySnapshot m_cachedSnapshot;
    LibrarySnapshot m_offlineSnapshot;
    LibrarySnapshot m_remoteSnapshot;
    ReconcileStats m_fetchStats;
    bool m_fetchCacheSaved = false;
    bool m_fetchOfflinePrepared = false;
    std::vector<TabData> m_fetchOfflineTabs;
    std::vector<MediaItem> m_fetchOfflineMovies;
    LibrarySnapshot m_fetchOfflineSnapshot;
    bool m_haveCachedSnapshot = false;
    SyncState m_syncState;
    bool m_forceHierarchyReconcile = false;
    LibrarySyncSchedule m_syncSchedule;
    bool m_libraryOffline = false;
    std::thread m_posterThread;
    std::mutex m_posterMutex;
    std::condition_variable m_posterWake;
    std::vector<PosterJob> m_pendingPosterJobs;
    bool m_stopPosterWorker = false;
    // Hierarchy discovery is deliberately a single background worker: it keeps
    // startup and the SDL thread free while placing a firm bound on requests.
    std::thread m_hierarchyThread;
    std::mutex m_hierarchyMutex;
    std::condition_variable m_hierarchyWake;
    std::vector<MediaItem> m_pendingHierarchyShows;
    std::uint64_t m_pendingHierarchyGeneration = 0;
    std::shared_ptr<std::atomic_bool> m_catalogGenerationCancellation;
    std::atomic<std::uint64_t> m_hierarchyGeneration{0};
    std::atomic<size_t> m_hierarchyCompleted{0}, m_hierarchyTotal{0};
    std::atomic<bool> m_hierarchyActive{false};
    std::atomic<bool> m_hierarchyOffline{false};
    bool m_stopHierarchyWorker = false;
    std::thread m_decodeThread;
    std::mutex m_decodeMutex;
    std::condition_variable m_decodeWake;
    struct DecodeJob { std::string key; PosterJob artwork; bool shows = false; ArtworkContext context = ArtworkContext::Unknown; };
    struct DecodeResult { std::string key; DecodedImage image; bool shows = false; bool cachePresent = false; };
    std::deque<DecodeJob> m_decodeJobs;
    std::deque<DecodeResult> m_decodeResults;
    std::set<std::string> m_decodeOutstanding;
    // Keys wanted by the current Shows viewport.  This is main-thread state;
    // queued jobs are reconciled with it under m_decodeMutex.
    std::set<std::string> m_activeShowsDecodeKeys;
    bool m_stopDecodeWorker = false;

    void startFetch();
    void requestFetch(Uint32 now);
    void finishFetch();
    void applyPresentationProjection();
    void restoreOnlinePresentation();
    void applyOfflineProjection();
    void prepareOfflineProjection();
    void startPosterSync(const LibrarySnapshot &snapshot);
    void queuePosterJobs(std::vector<PosterJob> jobs);
    void posterWorker();
    void startHierarchyCache(const LibrarySnapshot &snapshot, const LibrarySnapshot &previous,
                             const std::set<std::string> &changedSeries={});
    void hierarchyWorker();
    bool publishHierarchyCheckpoint(std::uint64_t generation);
    std::future<CatalogDbHierarchyWriteResult> submitCatalogHierarchy(
        const MediaItem &series, const std::vector<MediaItem> &seasons,
        const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
        std::uint64_t generation, bool complete,
        const std::shared_ptr<std::atomic_bool> &cancellation);
    std::string syncStatusText() const;
    void decodeWorker();
    void drainDecodedArtwork();
    void submitDecode(const MediaItem &item, bool highPriority=false,
                      bool shows=false);

    // Lightweight refresh used when returning to an already-loaded Home.
    std::thread m_resumeRefreshThread;
    std::atomic<bool> m_resumeRefreshDone{false};
    std::atomic<bool> m_resumeRefreshInFlight{false};
    bool m_resumeRefreshPending = false;
    bool m_resumeRefreshSucceeded = false;
    std::string m_resumeRefreshError;
    std::vector<MediaItem> m_resumeRefreshResult;
    bool m_resumeRefreshCacheSaved = false;

    void startResumeRefresh();
    void finishResumeRefresh();

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

    // Helpers
    const TabData &currentTab() const;
    bool activeTabNamed(const char *name) const;
    int tabIndex(const char *name) const;
    const MediaRow *currentRow() const;
    const MediaItem *currentItem() const;

    void clampNavigation();
    void drawTabBar(SDL_Surface *fb);
    void drawInfoPanel(SDL_Surface *fb);
    void drawRowList(SDL_Surface *fb);
    void drawCard(SDL_Surface *fb, int x, int y, int w, int h,
                  const MediaItem &item, bool selected);
    void drawPlaceholderTab(SDL_Surface *fb, const char *message);
    void drawBottomHints(SDL_Surface *fb);
    void drawLoadingState(SDL_Surface *fb);
    void drawErrorState(SDL_Surface *fb);
    void refreshDownloads();
    bool handleDownloadsAction(Action action);
    void drawDownloadsTab(SDL_Surface *fb);
    void drawSettingsTab(SDL_Surface *fb);

    // Selected artwork state (B5b)
    DecodedImage m_selectedArtwork;
    std::string  m_selectedArtworkId;      // "itemId:imageTag" identity key
    bool         m_selectedArtworkAttempted = false;

    void tryLoadSelectedArtwork();

    // Row artwork loading (B5d2a)
    void tryLoadOneRowArtwork();
    void evictRowArtworkIfNeeded();
    std::set<std::string> protectedRowArtworkKeys() const;
    void touchRowArtwork(const std::string &key);
    void storeDecodedRowArtwork(const std::string &key, DecodedImage image);
    void updateShowsDecodeWorkingSet();
    void drawMovieGrid(SDL_Surface *fb);
    void drawMoviePreview(SDL_Surface *fb);
    void drawMovieAlphabetRail(SDL_Surface *fb);
    void refreshMovieFilter();
    void rebuildShowsPresentation();
    void refreshShowsFilter();
    void resetMediaPaging();
    void requestMediaPage(MediaPageState &state);
    void finishMediaPage(MediaPageState &state);
    void updateMediaPaging();
    const MediaItem *showsSelectedItem() const;
    void clampShowsNavigation();
    void drawShowsGrid(SDL_Surface *fb);
    void drawShowsPreview(SDL_Surface *fb);
    void drawShowsAlphabetRail(SDL_Surface *fb);
    int moveMovieGridCompact(int index, int count, int deltaRow, int deltaCol) const;
    static std::vector<MediaItem> combineMovieViews(const std::vector<CachedLibraryView> &views);
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SCREEN_HPP
