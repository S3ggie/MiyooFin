#include "HomeScreen.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}
static void makeMediaTabsBounded(std::vector<TabData> &tabs);

void HomeScreen::publishCoordinatorHomeState(
    const std::vector<TabData> &tabs, const LibrarySnapshot &snapshot,
    bool contentValid, bool cachedSnapshotValid, bool offline, bool stale,
    bool continueValid, bool recentlyAddedValid, const std::string &error)
{
    if (!m_libraryCoordinator)
        return;
    library::HomeState state;
    state.scopeEpoch = m_catalogMetadata.scopeEpoch;
    state.catalogGeneration = m_topLevelSyncGeneration.load();
    state.offline = offline;
    state.stale = stale;
    state.contentValid = contentValid;
    state.cachedSnapshotValid = cachedSnapshotValid;
    state.tabs = tabs;
    state.cachedSnapshot = snapshot;
    state.continueValid = continueValid;
    state.recentlyAddedValid = recentlyAddedValid;
    state.continueWatching = snapshot.continueWatching;
    state.recentlyAdded = snapshot.recentlyAdded;
    state.error = error;
    const auto sync = m_libraryCoordinator->status();
    state.sync.inFlight = sync.inFlight;
    state.sync.startupInFlight = sync.startupInFlight;
    state.sync.fullSyncInFlight = sync.fullSyncInFlight;
    state.sync.cancelRequested = sync.cancelRequested;
    state.sync.success = sync.success;
    state.sync.generation = sync.generation;
    state.sync.lastSuccessfulMs = m_syncState.lastSuccessfulMs;
    state.sync.lastReconcileMs = m_syncState.lastReconcileMs;
    (void)m_libraryCoordinator->publishHomeState(std::move(state));
}

void HomeScreen::consumeCoordinatorHomeState()
{
    if (!m_libraryCoordinator)
        return;
    std::shared_ptr<const library::HomeState> state;
    if (!m_libraryCoordinator->takeHomeState(state) || !state
        || state->revision <= m_homeStateRevision)
        return;
    m_homeStateRevision = state->revision;
    applyCoordinatorHomeState(*state);
}

void HomeScreen::applyCoordinatorHomeState(const library::HomeState &state)
{
    // Only content/status comes from the coordinator.  Focus, selection,
    // scroll, and artwork remain untouched except for the existing bounds and
    // row-label reconciliation required after a content replacement.
    if (state.contentValid) {
        const std::string focusedLabel = focusedHomeRowLabel();
        const std::vector<TabData> previous = m_tabs;
        const int selected = m_activeTab;
        m_tabs = state.tabs;
        makeMediaTabsBounded(m_tabs);
        m_activeTab = transitionTabIndex(previous, selected, m_tabs);
        if (state.cachedSnapshotValid) {
            m_cachedSnapshot = state.cachedSnapshot;
            m_haveCachedSnapshot = true;
            if (state.offline)
                m_offlineSnapshot = state.cachedSnapshot;
        }
        restoreHomeRowFocus(focusedLabel);
        m_loadState = LoadState::Ready;
        clampNavigation();
    }
    if (state.cachedSnapshotValid) {
        m_cachedSnapshot = state.cachedSnapshot;
        m_haveCachedSnapshot = true;
    }
    if (state.continueValid) {
        updateContinueWatchingRow(m_tabs, state.continueWatching);
        m_cachedSnapshot.continueWatching = state.continueWatching;
        m_remoteSnapshot.continueWatching = state.continueWatching;
    }
    if (state.recentlyAddedValid) {
        updateRecentlyAddedRow(m_tabs, state.recentlyAdded);
        m_cachedSnapshot.recentlyAdded = state.recentlyAdded;
        m_remoteSnapshot.recentlyAdded = state.recentlyAdded;
    }
    m_libraryOffline = state.offline;
    if (!state.error.empty()) {
        m_fetchError = state.error;
        if (!state.contentValid && !m_haveCachedSnapshot)
            m_loadState = LoadState::Error;
    }
    if (state.sync.lastSuccessfulMs > 0)
        m_syncState.lastSuccessfulMs = state.sync.lastSuccessfulMs;
    if (state.sync.lastReconcileMs > 0)
        m_syncState.lastReconcileMs = state.sync.lastReconcileMs;
}

void HomeScreen::applyPresentationProjection() {
    // Called from finishFetch() after the offline worker path has populated
    // m_cachedSnapshot / m_haveCachedSnapshot.  Not called directly from
    // the Settings toggle (that path now drives startFetch() so the worker
    // builds the snapshot on a background thread).
    if (!m_haveCachedSnapshot) return;
    // Offline hierarchy comes from durable DownloadStore metadata.  The
    // legacy catalog is not a runtime authority; an empty catalog lets the
    // projection synthesize only the downloaded branches it needs.
    OfflineCatalogSnapshot catalog;
    OfflineLibraryProjection projection(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
    m_fetchOfflineMovies.clear();
    m_fetchOfflineSnapshot=m_cachedSnapshot;
    const std::vector<MediaItem> offlineMovies = projection.movies();
    std::set<std::string> offlineMovieIds;
    for (const auto &item : offlineMovies) offlineMovieIds.insert(item.id);
    for (auto &view : m_fetchOfflineSnapshot.movies) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items)
            if (offlineMovieIds.count(item.id)) filtered.push_back(item);
        view.items=std::move(filtered);
    }
    for (auto &view : m_fetchOfflineSnapshot.shows) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items) {
            if (projection.playable(item.id) || !projection.seasons(item.id).empty())
                filtered.push_back(item);
        }
        view.items=std::move(filtered);
    }
    m_fetchOfflineTabs=offlineTabsFromSnapshot(m_fetchOfflineSnapshot);
    for (auto &tab : m_fetchOfflineTabs) {
        if (tab.name == "Movies") tab.rows = {{"Movies", {}}};
        if (tab.name == "Shows") tab.rows = {{"Shows", {}}};
    }
    m_fetchOfflinePrepared=true;
    applyOfflineProjection();
}
void HomeScreen::restoreOnlinePresentation() {
    // Restores tabs from m_cachedSnapshot (the snapshot taken at last sync).
    // Note: the Settings offline-mode toggle no longer calls this directly;
    // it drives startFetch() instead.  This function remains available for
    // finishFetch() and other callers that need immediate tab restoration.
    if (!m_haveCachedSnapshot) return;
    const std::string focusedLabel = focusedHomeRowLabel();
    const std::vector<TabData> previous=m_tabs; const int selected=m_activeTab;
    m_tabs=tabsFromSnapshot(m_cachedSnapshot);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
    restoreHomeRowFocus(focusedLabel);
    resetMediaPaging();
    clampNavigation();
}
void HomeScreen::resetMediaPaging()
{
    auto reset = [](MediaPageState &state, const std::string &type, int letter) {
        if (state.cancellation)
            state.cancellation->store(true);
        state = MediaPageState{};
        state.type = type;
        state.letter = letter;
    };
    reset(m_moviePage, "movie", m_movieActiveLetter);
    reset(m_showPage, "show", m_showsActiveLetter);
    reset(m_animePage, "anime", m_showsActiveLetter);
    m_movieWindow.clear();
    m_showWindow.clear();
    m_animeWindow.clear();
    m_filteredShows.clear();
    m_filteredAnime.clear();
    if (const int movies = tabIndex("Movies"); movies >= 0)
        m_tabs[movies].rows = {{"Movies", {}}};
    if (const int shows = tabIndex("Shows"); shows >= 0)
        m_tabs[shows].rows = {{"Shows", {}}};
}
void HomeScreen::finishMediaPage(MediaPageState &state)
{
    if (!state.inFlight || !state.future.valid()
        || state.future.wait_for(std::chrono::milliseconds(0))
               != std::future_status::ready)
        return;
    const library::MediaPage result = state.future.get();
    state.inFlight = false;
    if (!result.success || result.cancelled || result.superseded)
        return;
    queuePosterJobs(planMediaPagePosterJobs(result.items), true);
    if (!m_firstMediaPageReadCompletedLogged) {
        m_firstMediaPageReadCompletedLogged = true;
        uiDiagnostics().log(
            "[HomeScreen] startup stage=first_read_media_page_ready");
    }
    if (!m_firstUsefulHomeLogged) {
        m_firstUsefulHomeLogged = true;
        uiDiagnostics().log(
            "[HomeScreen] startup stage=first_useful_home_ready");
    }
    auto &window = state.type == "movie"
        ? m_moviePage.items
        : state.type == "anime" ? m_animePage.items : m_showPage.items;
    if (state.replaceWindowOnNextPage) {
        window = result.items;
        state.replaceWindowOnNextPage = false;
        state.hasEarlier = false;
    } else {
        std::set<std::string> known;
        for (const auto &item : window)
            known.insert(item.id);
        for (const auto &item : result.items)
            if (known.insert(item.id).second)
                window.push_back(item);
    }
    static constexpr std::size_t kWindowLimit = 96;
    if (window.size() > kWindowLimit) {
        const std::size_t remove = window.size() - kWindowLimit;
        std::size_t removedGridItems = 0;
        if (state.type == "movie") {
            const int removedRows = static_cast<int>((remove + 7) / 8);
            m_rowScroll = m_rowScroll > removedRows
                ? m_rowScroll - removedRows : 0;
        } else {
            const auto &filtered = state.type == "anime"
                ? m_animeWindow : m_showWindow;
            for (std::size_t i = 0; i < remove; ++i) {
                for (const auto &item : filtered) {
                    if (item.id == window[i].id) {
                        ++removedGridItems;
                        break;
                    }
                }
            }
            const int removedRows = static_cast<int>((removedGridItems + 3) / 4);
            if (state.type == "anime")
                m_animeScroll = m_animeScroll > removedRows
                    ? m_animeScroll - removedRows : 0;
            else
                m_showScroll = m_showScroll > removedRows
                    ? m_showScroll - removedRows : 0;
        }
        window.erase(window.begin(),
                     window.begin() + static_cast<std::ptrdiff_t>(remove));
        state.hasEarlier = true;
    }
    state.next = result.next;
    state.hasMore = result.hasMore;
    if (state.type == "movie") {
        m_movieWindow = window;
        refreshMovieFilter();
        applyPendingDown(state);
    } else {
        {
            std::lock_guard<std::mutex> lock(m_fetchMutex);
            for (const auto &item : result.items) {
                const auto found = result.membershipsByItem.find(item.id);
                if (found == result.membershipsByItem.end())
                    continue;
                for (const auto &membership : found->second) {
                    if (isAnimeSeries(membership.viewName, item)) {
                        m_animeItemIds.insert(item.id);
                        break;
                    }
                }
            }
        }
        rebuildShowsPresentation();
        applyPendingDown(state);
    }
}
bool HomeScreen::liveChangeAffectsHome(
    const JellyfinLibraryChangeBatch &batch,
    const library::LiveLibraryChangeResult &result) const
{
    bool catalogItemAffectsHome = false;
    for (const auto &item : result.items)
        if (item.type == "movie" || item.type == "show") {
            catalogItemAffectsHome = true;
            break;
        }

    const auto contains = [](const std::vector<MediaItem> &items,
                             const std::string &id) {
        return std::find_if(items.begin(), items.end(),
                            [&](const MediaItem &item) {
                                return item.id == id;
                            }) != items.end();
    };
    const auto cachedHomeItem = [&](const std::string &id) {
        return contains(m_cachedSnapshot.continueWatching, id)
            || contains(m_cachedSnapshot.recentlyAdded, id)
            || contains(m_movieWindow, id)
            || contains(m_showWindow, id)
            || contains(m_animeWindow, id);
    };
    bool cachedHomeItemRemoved = false;
    for (const auto &id : result.removedIds)
        if (cachedHomeItem(id)) {
            cachedHomeItemRemoved = true;
            break;
        }
    return homeChangeNeedsPublication(batch.catchUpRequired,
                                      catalogItemAffectsHome,
                                      cachedHomeItemRemoved);
}
void HomeScreen::publishLiveCatalogItems(
    const library::LiveLibraryChangeResult &result)
{
    if (livePublicationIsNoop(result.items.empty(), result.removedIds.empty())) return;
    const auto replace = [&](std::vector<MediaItem> &items,
                             const MediaItem &changed) {
        for (auto &item : items) {
            if (item.id == changed.id) {
                item = changed;
                return;
            }
        }
    };
    const auto remove = [](std::vector<MediaItem> &items,
                           const std::string &id) {
        items.erase(std::remove_if(items.begin(), items.end(),
                                   [&](const MediaItem &item) {
                                       return item.id == id;
                                   }),
                    items.end());
    };
    for (const auto &item : result.items) {
        if (item.type == "movie") replace(m_movieWindow, item);
        if (item.type == "show") {
            replace(m_showWindow, item);
            replace(m_animeWindow, item);
        }
    }
    for (const auto &id : result.removedIds) {
        remove(m_movieWindow, id);
        remove(m_showWindow, id);
        remove(m_animeWindow, id);
    }
    refreshMovieFilter();
    rebuildShowsPresentation();
}
void HomeScreen::finishLiveChangeApply()
{
    if (!m_liveChangeThread.joinable()) return;
    if (m_libraryCoordinator
        && !m_libraryCoordinator->takeLiveChangeResult(
            m_liveChangeIdentity, m_liveChangeResult)) {
        // The worker can finish after coordinator stop/discard invalidates its
        // identity.  There is then no result to apply, but the completed
        // worker must still release Home's in-flight state.  Its thread is
        // joined by the next update (or by teardown) after this cleanup.
        if (!m_liveChangeDone.load()) return;
        m_liveChangeDone.store(false);
        m_liveChangeInFlight = false;
        m_liveChangeCancellation.reset();
        m_liveChangeResult = {};
        return;
    }
    // Thread join deferred to next startLiveChangeApply() or joinAllWorkers().
    // The Done atomic guarantees all result writes are visible.
    m_liveChangeDone.store(false);
    m_liveChangeInFlight = false;
    const auto batch = m_liveChangeBatch;
    const auto result = m_liveChangeResult;
    m_liveChangeCancellation.reset();
    if (result.success && liveChangeAffectsHome(batch, result)) {
        publishLiveCatalogItems(result);
        // Debounce: skip the rail refresh if the last successful refresh
        // completed within kHomeRailRefreshDebounceMs to avoid hammering the
        // server with ResumeItems+LatestItems pairs on rapid live changes.
        const std::int64_t nowMs = wallClockMs();
        if (!homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshCompletedMs)
            && !homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshAttemptMs)) {
            m_homeSyncActive = true;
            startHomeRailRefresh();
        }
    }
}
void HomeScreen::finishHomeRailRefresh()
{
    m_homeRailRefreshDone.store(false);
    m_homeRailRefreshInFlight = false;
    if (m_homeRailRefreshSucceeded) {
        m_lastHomeRailRefreshCompletedMs = wallClockMs();
        const std::string focusedLabel = focusedHomeRowLabel();
        if (m_homeRailContinueValid) {
            updateContinueWatchingRow(m_tabs, m_homeRailContinueWatching);
            m_cachedSnapshot.continueWatching = m_homeRailContinueWatching;
        }
        if (m_homeRailRecentValid) {
            updateRecentlyAddedRow(m_tabs, m_homeRailRecentlyAdded);
            m_cachedSnapshot.recentlyAdded = m_homeRailRecentlyAdded;
        }
        restoreHomeRowFocus(focusedLabel);
        queuePosterJobs(planHomeRailPosterJobs(
            m_homeRailContinueWatching, m_homeRailRecentlyAdded), true);
    } else if (!m_homeRailRefreshError.empty()) {
        std::printf("[HomeScreen] live Home rail refresh failed: %s\n",
                    m_homeRailRefreshError.c_str());
    }
    m_homeSyncActive = false;
    if (m_homeRailRefreshSucceeded)
        clampNavigation();
    if (m_homeRailRefreshPending) {
        m_homeRailRefreshPending = false;
        startHomeRailRefresh();
    }
}
void HomeScreen::finishSafetyReconcile()
{
    if (!m_safetyReconcileInFlight || !m_libraryCoordinator)
        return;
    library::SafetyReconcileResult result;
    if (!m_libraryCoordinator->takeSafetyReconcileResult(result))
        return;
    // Thread join deferred to LibraryCoordinator teardown; the UI thread only
    // consumes the completed result here.
    m_safetyReconcileInFlight = false;
    if (result.generation > m_topLevelSyncGeneration.load())
        m_topLevelSyncGeneration.store(result.generation);
    if (result.lastSuccessfulMs > 0) {
        m_syncState.lastSuccessfulMs = result.lastSuccessfulMs;
        m_syncState.lastReconcileMs = result.lastReconcileMs;
    }
    m_lastSafetyReconcileMs = result.lastReconcileMs > 0
        ? result.lastReconcileMs : wallClockMs();
    if (!result.success && !result.message.empty())
        std::printf("[HomeScreen] safety reconciliation failed: %s\n",
                    result.message.c_str());
}

static void makeMediaTabsBounded(std::vector<TabData> &tabs)
{
    for (auto &tab : tabs) {
        if (tab.name == "Movies") tab.rows = {{"Movies", {}}};
        if (tab.name == "Shows") tab.rows = {{"Shows", {}}};
    }
}
void HomeScreen::finishFetch()
{
    if (!m_fetchReady.load())
        return;
    if (!m_fetchPublished) {
        if(!m_fetchError.empty()){m_libraryOffline=m_haveCachedSnapshot;if(m_libraryOffline)applyOfflineProjection();if(!m_haveCachedSnapshot && !m_offlineModeFetchPending)m_loadState=LoadState::Error;printf("[HomeScreen] Fetch failed: %s\n",m_fetchError.c_str());m_fetchPublished=true;}
        else {
            std::vector<TabData> publishedTabs;
            {
                std::lock_guard<std::mutex> lock(m_fetchMutex);
                publishedTabs = std::move(m_fetchResult);
            }
            const HomeMediaWindows warmWindows = mediaWindowsFromTabs(publishedTabs);
            const std::string focusedLabel = focusedHomeRowLabel();
            const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;m_tabs=std::move(publishedTabs);makeMediaTabsBounded(m_tabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_libraryOffline=false;if(m_session.manualOfflineMode)applyPresentationProjection();else resetMediaPaging();restoreHomeRowFocus(focusedLabel);m_loadState=LoadState::Ready;clampNavigation();printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n",m_tabs.size(),m_fetchStats.added,m_fetchStats.changed);uiDiagnostics().log("[HomeScreen] startup stage=loading_state_cleared");m_fetchPublished = true;
            if (!warmWindows.movies.empty()) {
                m_moviePage.items = warmWindows.movies;
                m_movieWindow = warmWindows.movies;
                refreshMovieFilter();
            }
            if (!warmWindows.shows.empty()) {
                m_showPage.items = warmWindows.shows;
                rebuildShowsPresentation();
            }
        }
    }
    // Incremental rail publication: apply home rails as soon as the fetch
    // worker has them, without waiting for the full population walk.
    // Read from the worker-owned startup buffer under m_fetchMutex to
    // avoid data races with the refresh thread's own rail members.
    if (m_homeRailsReady.load() && !m_homeRailsApplied) {
        m_homeRailsApplied = true;
        std::vector<MediaItem> railCW;
        std::vector<MediaItem> railRA;
        bool cwValid = false;
        bool raValid = false;
        {
            std::lock_guard<std::mutex> lock(m_fetchMutex);
            railCW = std::move(m_startupRailCW);
            railRA = std::move(m_startupRailRA);
            cwValid = m_startupRailCWValid;
            raValid = m_startupRailRAValid;
        }
        {
            const std::string focusedLabel = focusedHomeRowLabel();
            if (cwValid) {
                updateContinueWatchingRow(m_tabs, railCW);
                m_cachedSnapshot.continueWatching = railCW;
            }
            if (raValid) {
                updateRecentlyAddedRow(m_tabs, railRA);
                m_cachedSnapshot.recentlyAdded = railRA;
            }
            restoreHomeRowFocus(focusedLabel);
        }
        queuePosterJobs(planHomeRailPosterJobs(railCW, railRA), true);
        clampNavigation();
    }
    if (m_fetchComplete.load() && !m_fetchPostFinalizeApplied) {
        m_fetchPostFinalizeApplied = true;
        // Post-finalize tab rebuild: on a cold start the initial
        // first-bounded-page publish used empty movie/show lists.
        // The worker rebuilt m_fetchResult after finalize; apply it.
        // Mirror the first-publish pattern: copy data out under a
        // scoped lock, release the lock, THEN mutate windows and
        // call presentation functions that themselves lock m_fetchMutex.
        std::vector<TabData> rebuiltTabs;
        {
            std::lock_guard<std::mutex> lock(m_fetchMutex);
            rebuiltTabs = std::move(m_fetchResult);
        }
        const std::string focusedLabel = focusedHomeRowLabel();
        if (m_fetchError.empty() && !rebuiltTabs.empty()) {
            const HomeMediaWindows warmWindows =
                mediaWindowsFromTabs(rebuiltTabs);
            const std::vector<TabData> previous = m_tabs;
            const int selected = m_activeTab;
            m_tabs = std::move(rebuiltTabs);
            makeMediaTabsBounded(m_tabs);
            m_activeTab = transitionTabIndex(previous, selected, m_tabs);
            if (!warmWindows.movies.empty()) {
                m_moviePage.items = warmWindows.movies;
                m_movieWindow = warmWindows.movies;
                refreshMovieFilter();
            }
            if (!warmWindows.shows.empty()) {
                m_showPage.items = warmWindows.shows;
                rebuildShowsPresentation();
            }
        }
        if (m_fetchCacheSaved) {
            m_cachedSnapshot=m_remoteSnapshot;
            m_haveCachedSnapshot=true;
            // A user can enable manual offline mode while the first bounded
            // sync is still completing.  The earlier projection attempt has
            // no snapshot to consume, so apply it once the snapshot is ready.
            if (m_session.manualOfflineMode)
                applyPresentationProjection();
        }
        updateContinueWatchingRow(m_tabs, m_remoteSnapshot.continueWatching);
        updateRecentlyAddedRow(m_tabs, m_remoteSnapshot.recentlyAdded);
        restoreHomeRowFocus(focusedLabel);
        clampNavigation();
        // Mark the sync schedule complete only after the fetch thread has
        // finished (Done atomic set), which means the top-level generation
        // has committed (finalize) or been safely aborted.  This prevents
        // the live-change path from starting a competing top-level sync
        // prematurely.  The thread is joined later in joinAllWorkers().
        m_syncSchedule.complete(SDL_GetTicks(), m_fetchError.empty());
        if (m_fetchError.empty()) {
            m_lastSafetyReconcileMs = wallClockMs();
        } else {
            m_lastSafetyReconcileMs = 0;
        }
        // If the user toggled offline mode while this fetch was in-flight,
        // the fetched tabs may not match the current session mode.  Re-fetch
        // so the worker takes the correct path (offline snapshot or full
        // online sync) and publishes tabs that match the toggled mode.
        if (m_offlineModeFetchPending) {
            m_offlineModeFetchPending = false;
            startFetch();
        }
        m_fetchDone.store(false);
    }
}
}
