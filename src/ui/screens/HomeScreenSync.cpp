#include "HomeScreen.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include <chrono>
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

void HomeScreen::requestMediaPage(MediaPageState& state)
{
    if (state.inFlight || !state.hasMore)
        return;
    state.cancellation = std::make_shared<std::atomic_bool>(false);
    if (presentationOffline()) {
        std::promise<library::MediaPage> page;
        page.set_value(offlineMediaPage(m_offlineSnapshot, state.type, state.letter,
                                        state.type == "anime" ? 64 : 24, state.next));
        state.future = page.get_future();
        state.inFlight = true;
        return;
    }
    if (!m_libraryQuery)
        return;
    if (state.type == "movie")
        state.future = m_libraryQuery->movies(state.letter, 24, state.next, state.cancellation);
    else if (state.type == "anime")
        state.future = m_libraryQuery->anime(state.letter, 64, state.next);
    else
        state.future = m_libraryQuery->shows(state.letter, 24, state.next, state.cancellation);
    state.inFlight = true;
    if (!m_firstMediaPageReadLogged) {
        m_firstMediaPageReadLogged = true;
        uiDiagnostics().log("[HomeScreen] startup stage=first_read_media_page_requested");
    }
}

void HomeScreen::requestEarlierMediaPage(MediaPageState& state)
{
    if (!m_libraryQuery || state.inFlight || !state.hasEarlier)
        return;
    const std::string type = state.type;
    const int letter = state.letter;
    if (state.cancellation)
        state.cancellation->store(true);
    state = MediaPageState{};
    state.type = type;
    state.letter = letter;
    state.replaceWindowOnNextPage = true;
    requestMediaPage(state);
}

void HomeScreen::updateMediaPaging()
{
    finishMediaPage(m_moviePage);
    finishMediaPage(m_showPage);
    finishMediaPage(m_animePage);
    if (activeTabNamed("Movies")) {
        const auto& rows = m_tabs[tabIndex("Movies")].rows;
        const auto& items = rows.empty() ? m_movieWindow : rows[0].items;
        if (items.empty() || m_activeCard + 8 >= static_cast<int>(items.size()))
            requestMediaPage(m_moviePage);
    } else if (activeTabNamed("Shows")) {
        const int showCount = static_cast<int>(m_filteredShows.size());
        const int animeCount = static_cast<int>(m_filteredAnime.size());
        if (showCount == 0 || m_showSelected + 4 >= showCount)
            requestMediaPage(m_showPage);
        if (animeCount == 0 || m_animeSelected + 4 >= animeCount)
            requestMediaPage(m_animePage);
    }
}

void HomeScreen::updateLiveLibraryChanges()
{
    if (!m_libraryCoordinator || presentationOffline())
        return;
    library::LiveLibraryChangeResult result;
    if (!m_libraryCoordinator->takeLiveChangeResult(result))
        return;
    if (result.success && liveChangeAffectsHome(result))
        publishLiveCatalogItems(result);
    if (result.success && result.userDataChanged) {
        const std::int64_t nowMs = wallClockMs();
        if (!homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshCompletedMs) &&
            !homeRailRefreshDebounced(nowMs, m_lastHomeRailRefreshAttemptMs)) {
            m_homeSyncActive = true;
            startHomeRailRefresh();
        }
    }
}

void HomeScreen::startHomeRailRefresh()
{
    if (m_homeRailRefreshInFlight) {
        m_homeRailRefreshPending = true;
        return;
    }
    if (!m_libraryFetch || !m_libraryFetch->requestHomeRailRefresh())
        return;
    m_homeRailRefreshInFlight = true;
    m_homeRailRefreshSucceeded = false;
    m_lastHomeRailRefreshAttemptMs = wallClockMs();
    m_homeRailContinueValid = false;
    m_homeRailRecentValid = false;
    m_homeRailContinueWatching.clear();
    m_homeRailRecentlyAdded.clear();
    m_homeRailRefreshError.clear();
}

bool HomeScreen::startFetch()
{
    if (!m_libraryFetch)
        return false;
    m_libraryFetch->setManualOfflineMode(m_session.manualOfflineMode);
    const bool started = m_libraryFetch->startFetch(
        m_tabs, m_cachedSnapshot, m_remoteSnapshot, m_haveCachedSnapshot, m_libraryOffline,
        m_loadState == LoadState::Ready && !m_tabs.empty(), m_animeItemIds);
    if (started) {
        if (m_artworkController)
            m_artworkController->setLowPriorityDeferred(true);
        m_animeItemIds.clear();
        m_fetchPublished = false;
        m_fetchPostFinalizeApplied = false;
        m_fetchFailureRestored = false;
        m_fetchError.clear();
        m_homeRailsApplied = false;
        m_fetchRailCW.clear();
        m_fetchRailRA.clear();
        m_fetchRailCWValid = false;
        m_fetchRailRAValid = false;
    }
    return started;
}

void HomeScreen::requestFetch(Uint32 now)
{
    if (m_syncSchedule.request(now))
        startFetch();
}

} // namespace miyoofin
