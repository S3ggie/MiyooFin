#include "HomeScreen.hpp"
#include "../../playback/OfflineLibraryProjection.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}

void HomeScreen::applyPresentationProjection() {
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
    if (!m_haveCachedSnapshot) return;
    const std::vector<TabData> previous=m_tabs; const int selected=m_activeTab;
    m_tabs=tabsFromSnapshot(m_cachedSnapshot);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
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
    m_liveChangeThread.join();
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
        if (!homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshCompletedMs)) {
            m_homeSyncActive = true;
            startHomeRailRefresh();
        }
    }
}
void HomeScreen::finishHomeRailRefresh()
{
    if (!m_homeRailRefreshThread.joinable()) return;
    m_homeRailRefreshThread.join();
    m_homeRailRefreshDone.store(false);
    m_homeRailRefreshInFlight = false;
    if (m_homeRailRefreshSucceeded) {
        m_lastHomeRailRefreshCompletedMs = wallClockMs();
        if (m_homeRailContinueValid) {
            updateContinueWatchingRow(m_tabs, m_homeRailContinueWatching);
            m_cachedSnapshot.continueWatching = m_homeRailContinueWatching;
        }
        if (m_homeRailRecentValid) {
            updateRecentlyAddedRow(m_tabs, m_homeRailRecentlyAdded);
            m_cachedSnapshot.recentlyAdded = m_homeRailRecentlyAdded;
        }
        queuePosterJobs(planHomeRailPosterJobs(
            m_homeRailContinueWatching, m_homeRailRecentlyAdded), true);
    } else if (!m_homeRailRefreshError.empty()) {
        std::printf("[HomeScreen] live Home rail refresh failed: %s\n",
                    m_homeRailRefreshError.c_str());
    }
    m_homeRailRefreshCancellation.reset();
    m_homeSyncActive = false;
    clampNavigation();
    if (m_homeRailRefreshPending) {
        m_homeRailRefreshPending = false;
        startHomeRailRefresh();
    }
}
void HomeScreen::finishSafetyReconcile()
{
    if (!m_safetyReconcileThread.joinable()) return;
    m_safetyReconcileThread.join();
    m_safetyReconcileDone.store(false);
    m_safetyReconcileInFlight = false;
    m_lastSafetyReconcileMs = wallClockMs();
    if (!m_safetyReconcileError.empty())
        std::printf("[HomeScreen] safety reconciliation failed: %s\n",
                    m_safetyReconcileError.c_str());
    m_safetyReconcileCancellation.reset();
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
        if(!m_fetchError.empty()){m_libraryOffline=m_haveCachedSnapshot;if(m_libraryOffline)applyOfflineProjection();if(!m_haveCachedSnapshot)m_loadState=LoadState::Error;printf("[HomeScreen] Fetch failed: %s\n",m_fetchError.c_str());m_fetchPublished=true;}
        else {
            std::vector<TabData> publishedTabs;
            {
                std::lock_guard<std::mutex> lock(m_fetchMutex);
                publishedTabs = std::move(m_fetchResult);
            }
            const HomeMediaWindows warmWindows = mediaWindowsFromTabs(publishedTabs);
            const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;m_tabs=std::move(publishedTabs);makeMediaTabsBounded(m_tabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_libraryOffline=false;if(m_session.manualOfflineMode)applyPresentationProjection();else resetMediaPaging();m_loadState=LoadState::Ready;clampNavigation();printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n",m_tabs.size(),m_fetchStats.added,m_fetchStats.changed);uiDiagnostics().log("[HomeScreen] startup stage=loading_state_cleared");m_fetchPublished = true;
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
    if (m_fetchComplete.load() && m_fetchThread.joinable()) {
        m_fetchThread.join();
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
        // Mark the sync schedule complete only after the fetch thread has
        // joined, which means the top-level generation has committed
        // (finalize) or been safely aborted.  This prevents the live-change
        // path from starting a competing top-level sync prematurely.
        m_syncSchedule.complete(SDL_GetTicks(), m_fetchError.empty());
        if (m_fetchError.empty()) {
            m_lastSafetyReconcileMs = wallClockMs();
        } else {
            m_lastSafetyReconcileMs = 0;
        }
        m_fetchDone.store(false);
    }
}
}
