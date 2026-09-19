#include "HomeScreen.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../update/AppDir.hpp"
#include <cstdio>
#include <ctime>

namespace miyoofin {

static std::int64_t homeWallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

HomeScreen::HomeScreen(const Session &session,
                       std::shared_ptr<DownloadManager> downloads,
                       std::shared_ptr<CatalogDb> catalogDb,
                       std::uint64_t catalogScopeEpoch,
                       std::shared_ptr<library::LibrarySync> librarySync,
                       std::shared_ptr<library::LibraryQuery> libraryQuery,
                       std::shared_ptr<library::LibraryCoordinator> libraryCoordinator)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_downloads(std::move(downloads))
    , m_catalogDb(std::move(catalogDb))
    , m_librarySync(std::move(librarySync))
    , m_libraryQuery(std::move(libraryQuery))
    , m_libraryCoordinator(std::move(libraryCoordinator))
    , m_catalogMetadata()
    , m_userName(session.userName)
{
    m_catalogMetadata.scopeEpoch = catalogScopeEpoch;
    // Placeholder tabs until fetch completes
    m_tabs.push_back({"Home", {{"", {}}}});
    m_tabs.push_back({"Movies", {{"", {}}}});
    m_tabs.push_back({"Shows", {{"", {}}}});
    m_tabs.push_back({"Downloads", {{"", {}}}});
    m_tabs.push_back({"Settings", {{"", {}}}});
    for (int i = 0; i < kPosterThreads; ++i)
        m_posterThreads.emplace_back(&HomeScreen::posterWorker, this);
    m_hierarchyThread = std::thread(&HomeScreen::hierarchyWorker, this);
    m_decodeThread = std::thread(&HomeScreen::decodeWorker, this);

    // Enable OTA updates if app directory can be resolved.
    {
        std::string dir;
        if (appDir(dir))
            m_updateManager.enable(std::move(dir));
    }
}

HomeScreen::~HomeScreen()
{
    requestStopAllWorkers();
    joinAllWorkers();
    freeAllCardSurfaces();
}

void HomeScreen::requestStopAllWorkers() noexcept
{
    if (m_moviePage.cancellation) m_moviePage.cancellation->store(true);
    if (m_showPage.cancellation) m_showPage.cancellation->store(true);
    if (m_animePage.cancellation) m_animePage.cancellation->store(true);
    if (m_fetchCancellation) m_fetchCancellation->store(true);
    if (m_libraryCoordinator) m_libraryCoordinator->cancelStartupSync();
    if (m_liveChangeCancellation)
        m_liveChangeCancellation->store(true);
    if (m_safetyReconcileCancellation)
        m_safetyReconcileCancellation->store(true);
    if (m_homeRailRefreshCancellation)
        m_homeRailRefreshCancellation->store(true);
    m_updateManager.cancel();
    {
        std::lock_guard<std::mutex> lock(m_hierarchyMutex);
        m_stopHierarchyWorker = true;
        if (m_catalogGenerationCancellation)
            m_catalogGenerationCancellation->store(true);
    }
    m_hierarchyWake.notify_one();
    { std::lock_guard<std::mutex> lock(m_posterMutex); m_stopPosterWorker = true; }
    m_posterWake.notify_all();
    { std::lock_guard<std::mutex> lock(m_decodeMutex); m_stopDecodeWorker = true; }
    m_decodeWake.notify_one();
}

void HomeScreen::joinAllWorkers()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();
    if (m_downloadRefreshThread.joinable())
        m_downloadRefreshThread.join();
    if (m_liveChangeThread.joinable())
        m_liveChangeThread.join();
    if (m_safetyReconcileThread.joinable())
        m_safetyReconcileThread.join();
    if (m_homeRailRefreshThread.joinable())
        m_homeRailRefreshThread.join();
    if (m_hierarchyThread.joinable()) m_hierarchyThread.join();
    for (auto &thread : m_posterThreads)
        if (thread.joinable()) thread.join();
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

// EXIT-ONLY: permanently requests all background workers stop (flags are
// never-reset).  Only safe on the screen-teardown / app-exit path where the
// screen is destroyed immediately afterwards.  Do NOT call mid-session.
void HomeScreen::cancelAsyncWork() noexcept
{
    requestStopAllWorkers();
}

void HomeScreen::updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items) { miyoofin::updateContinueWatchingRow(tabs, items); }
std::vector<TabData> HomeScreen::tabsFromSnapshot(const LibrarySnapshot &s) { return miyoofin::tabsFromSnapshot(s); }
std::vector<TabData> HomeScreen::offlineTabsFromSnapshot(const LibrarySnapshot &s) { return miyoofin::offlineTabsFromSnapshot(s); }
library::MediaPage HomeScreen::offlineMediaPage(const LibrarySnapshot &snapshot,
                                                const std::string &type,
                                                int alphabetLetter,
                                                std::size_t limit,
                                                const CatalogDbPageCursor &after)
{
    std::vector<MediaItem> items;
    if (type == "movie") {
        items = combineMovieViews(snapshot.movies);
    } else {
        const ShowsPresentation presentation = makeShowsPresentation(snapshot.shows);
        items = type == "anime" ? presentation.anime : presentation.shows;
    }
    items.erase(std::remove_if(items.begin(), items.end(),
                               [alphabetLetter](const MediaItem &item) {
                                   return !matchesAlphabetFilter(item.title, alphabetLetter);
                               }),
                items.end());
    std::sort(items.begin(), items.end(), organizationalLess);

    std::size_t start = 0;
    if (after.valid) {
        while (start < items.size() && items[start].id != after.id) ++start;
        if (start < items.size()) ++start;
    }
    const std::size_t pageLimit = std::max<std::size_t>(1, limit);
    const std::size_t end = std::min(items.size(), start + pageLimit);
    library::MediaPage page;
    page.success = true;
    page.hasMore = end < items.size();
    page.items.assign(items.begin() + static_cast<std::ptrdiff_t>(start),
                      items.begin() + static_cast<std::ptrdiff_t>(end));
    if (!page.items.empty()) {
        const MediaItem &last = page.items.back();
        page.next.valid = page.hasMore;
        page.next.sortKey = organizationalSortKey(last.title);
        page.next.title = last.title;
        page.next.id = last.id;
    }
    return page;
}
std::vector<std::string> HomeScreen::tabNames(const std::vector<TabData> &tabs) { return miyoofin::tabNames(tabs); }
int HomeScreen::transitionTabIndex(const std::vector<TabData> &from, int selected, const std::vector<TabData> &to) { return miyoofin::transitionTabIndex(from, selected, to); }

const char *HomeScreen::lastApiRouteValue()
{
    return RouteStatus::label(RouteStatus::latest());
}

std::vector<MediaItem> HomeScreen::combineMovieViews(const std::vector<CachedLibraryView> &views)
{ return miyoofin::combineMovieViews(views); }

void HomeScreen::rebuildShowsPresentation()
{
    std::vector<MediaItem> rawWindow = m_showPage.items;
    std::set<std::string> rawIds;
    for (const auto &item : rawWindow) rawIds.insert(item.id);
    for (const auto &item : m_animePage.items)
        if (rawIds.insert(item.id).second) rawWindow.push_back(item);
    m_showWindow.clear();
    m_animeWindow.clear();
    const auto &views = (presentationOffline() ? m_offlineSnapshot
                                                : m_cachedSnapshot).shows;
    std::set<std::string> animeItemIds;
    {
        std::lock_guard<std::mutex> lock(m_fetchMutex);
        animeItemIds = m_animeItemIds;
    }
    std::set<std::string> seenShows;
    std::set<std::string> seenAnime;
    for (const auto &item : rawWindow) {
        bool anime = animeItemIds.count(item.id) != 0;
        for (const auto &view : views) {
            if (anime) break;
            auto found = std::find_if(view.items.begin(), view.items.end(),
                                      [&](const MediaItem &candidate) {
                                          return candidate.id == item.id;
                                      });
            if (found != view.items.end() && isAnimeSeries(view.name, *found)) {
                anime = true;
                break;
            }
        }
        if (anime) {
            if (seenAnime.insert(item.id).second)
                m_animeWindow.push_back(item);
        } else if (seenShows.insert(item.id).second) {
            m_showWindow.push_back(item);
        }
    }
    std::sort(m_showWindow.begin(), m_showWindow.end(), organizationalLess);
    std::sort(m_animeWindow.begin(), m_animeWindow.end(), organizationalLess);
    refreshShowsFilter();
}

void HomeScreen::enter()
{
    printf("[HomeScreen] enter (tab=%d) user=%s\n", m_activeTab,
           m_userName.c_str());
    uiDiagnostics().log("[HomeScreen] startup stage=home_entered");
    if (m_loadState == LoadState::Loading) {
        if (!m_fetchDone) {
            // The SQLite checkpoint is read by startFetch's worker before any
            // ChangedHierarchy request.  The first bounded page publishes the
            // minimum Home data; remaining population stays in the worker.
            m_forceHierarchyReconcile=true;
            requestFetch(SDL_GetTicks());
        }
    }
    else if (m_loadState == LoadState::Ready) {
        if (m_resumeRefreshInFlight)
            m_resumeRefreshPending = true;
        else
            startResumeRefresh();
    }
}

void HomeScreen::leave()
{
    printf("[HomeScreen] leave\n");
}

void HomeScreen::update(Uint32 dt)
{
    if (!m_catalogScopeReadyLogged && m_catalogDb) {
        const CatalogDbScopeState state = m_catalogDb->scopeState();
        if (state.ready) {
            m_catalogScopeReadyLogged = true;
            uiDiagnostics().log("[HomeScreen] startup stage=catalog_scope_ready");
        }
    }
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) { m_logoutTimer = 0; m_logoutArmed = false; }
        else m_logoutTimer -= dt;
    }
    if (m_fetchReady.load()) {
        UiDiagnostics::Scope scope("HomeScreen::publishLibraryResult");
        finishFetch();
    }
    if (m_loadState == LoadState::Ready && m_resumeRefreshDone) {
        UiDiagnostics::Scope scope("HomeScreen::publishResumeResult");
        finishResumeRefresh();
    }
    if (m_loadState == LoadState::Ready) {
        if (m_liveChangeDone.load()) finishLiveChangeApply();
        updateLiveLibraryChanges();
        if (m_homeRailRefreshDone.load()) finishHomeRailRefresh();
        if (m_safetyReconcileDone.load()) finishSafetyReconcile();
        if (!m_session.manualOfflineMode && m_lastSafetyReconcileMs > 0
            && homeWallClockMs() - m_lastSafetyReconcileMs
                >= 24LL * 60 * 60 * 1000)
            startSafetyReconcile();
    }
    if (m_loadState == LoadState::Ready)
        updateMediaPaging();
    if (m_downloadRefreshTimer > dt) m_downloadRefreshTimer-=dt; else m_downloadRefreshTimer=0;
    if (m_loadState == LoadState::Ready && (activeTabNamed("Downloads") || activeTabNamed("Settings"))) refreshDownloads();
    // Reconcile before draining: an in-flight decode can complete between a
    // directional input and this update, so it must see the new viewport.
    if (m_loadState == LoadState::Ready)
        { UiDiagnostics::Scope scope("HomeScreen::updateArtworkWorkingSet"); updateShowsDecodeWorkingSet(); }
    { UiDiagnostics::Scope scope("HomeScreen::publishDecodedArtwork"); drainDecodedArtwork(); }

    // Attempt selected artwork load (identity guard prevents repeats)
    if (m_loadState == LoadState::Ready)
        { UiDiagnostics::Scope scope("HomeScreen::queueSelectedArtwork"); tryLoadSelectedArtwork(); }

    // Queue missing visible row artwork for background decode.
    if (m_loadState == LoadState::Ready)
        { UiDiagnostics::Scope scope("HomeScreen::queueVisibleArtwork"); tryLoadOneRowArtwork(); }

    // Poll OTA update manager for stage changes.
    m_updateManager.pollDone();
    m_updateSnapshot = m_updateManager.snapshot();
}



} // namespace miyoofin
