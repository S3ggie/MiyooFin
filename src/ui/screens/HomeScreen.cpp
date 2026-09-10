#include "HomeScreen.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../app/UiDiagnostics.hpp"
#include <cstdio>
#include <ctime>

namespace miyoofin {

static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}

static std::future<CatalogDbHierarchyWriteResult> rejectedCatalogHierarchy(
    const char *message)
{
    std::promise<CatalogDbHierarchyWriteResult> promise;
    CatalogDbHierarchyWriteResult result;
    result.error = CatalogDbErrorCategory::ConfigurationFailed;
    result.message = message;
    promise.set_value(std::move(result));
    return promise.get_future();
}

HomeScreen::HomeScreen(const Session &session,
                       std::shared_ptr<DownloadManager> downloads,
                       std::shared_ptr<CatalogDb> catalogDb,
                       std::uint64_t catalogScopeEpoch)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_downloads(std::move(downloads))
    , m_catalogDb(std::move(catalogDb))
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
    m_posterThread = std::thread(&HomeScreen::posterWorker, this);
    m_hierarchyThread = std::thread(&HomeScreen::hierarchyWorker, this);
    m_decodeThread = std::thread(&HomeScreen::decodeWorker, this);
}

HomeScreen::~HomeScreen()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();
    if (m_downloadRefreshThread.joinable())
        m_downloadRefreshThread.join();
    { std::lock_guard<std::mutex> lock(m_hierarchyMutex);
      m_stopHierarchyWorker = true;
      if (m_catalogGenerationCancellation)
          m_catalogGenerationCancellation->store(true);
    }
    m_hierarchyWake.notify_one();
    if (m_hierarchyThread.joinable()) m_hierarchyThread.join();
    { std::lock_guard<std::mutex> lock(m_posterMutex); m_stopPosterWorker = true; }
    m_posterWake.notify_one();
    if (m_posterThread.joinable()) m_posterThread.join();
    { std::lock_guard<std::mutex> lock(m_decodeMutex); m_stopDecodeWorker = true; }
    m_decodeWake.notify_one();
    if (m_decodeThread.joinable()) m_decodeThread.join();
}

void HomeScreen::updateContinueWatchingRow(std::vector<TabData> &tabs, const std::vector<MediaItem> &items) { miyoofin::updateContinueWatchingRow(tabs, items); }
std::vector<TabData> HomeScreen::tabsFromSnapshot(const LibrarySnapshot &s) { return miyoofin::tabsFromSnapshot(s); }
std::vector<TabData> HomeScreen::offlineTabsFromSnapshot(const LibrarySnapshot &s) { return miyoofin::offlineTabsFromSnapshot(s); }
std::vector<std::string> HomeScreen::tabNames(const std::vector<TabData> &tabs) { return miyoofin::tabNames(tabs); }
int HomeScreen::transitionTabIndex(const std::vector<TabData> &from, int selected, const std::vector<TabData> &to) { return miyoofin::transitionTabIndex(from, selected, to); }

const char *HomeScreen::lastApiRouteValue()
{
    return RouteStatus::label(RouteStatus::latest());
}

std::future<CatalogDbHierarchyWriteResult>
HomeScreen::submitCatalogHierarchyForTest(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, bool complete)
{
    return submitCatalogHierarchy(series, seasons, episodesBySeason,
                                  generation, complete, {});
}

std::future<CatalogDbHierarchyWriteResult> HomeScreen::submitCatalogHierarchy(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, bool complete,
    const std::shared_ptr<std::atomic_bool> &cancellation)
{
    if (!complete)
        return rejectedCatalogHierarchy(
            "incomplete hierarchy is not eligible for CatalogDb commit");
    if (!m_catalogDb)
        return rejectedCatalogHierarchy("CatalogDb service is unavailable");
    CatalogDbJobMetadata metadata = m_catalogMetadata;
    metadata.cancellation = cancellation;
    return m_catalogDb->stageSeriesHierarchy(
        series, seasons, episodesBySeason, generation, wallClockMs(), true,
        metadata);
}

std::vector<MediaItem> HomeScreen::combineMovieViews(const std::vector<CachedLibraryView> &views)
{ return miyoofin::combineMovieViews(views); }

void HomeScreen::rebuildShowsPresentation() { ShowsPresentation p=makeShowsPresentation((presentationOffline()?m_offlineSnapshot:m_cachedSnapshot).shows); m_showMaster=std::move(p.shows); m_animeMaster=std::move(p.anime); refreshShowsFilter(); }

void HomeScreen::enter()
{
    printf("[HomeScreen] enter (tab=%d) user=%s\n", m_activeTab,
           m_userName.c_str());
    if (m_loadState == LoadState::Loading && !m_fetchDone) {
        // The SQLite checkpoint is read by startFetch's worker before any
        // ChangedHierarchy request.  Until that result arrives, remain
        // conservative and perform the normal online fetch.
        m_forceHierarchyReconcile=true;
        requestFetch(SDL_GetTicks());
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
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) { m_logoutTimer = 0; m_logoutArmed = false; }
        else m_logoutTimer -= dt;
    }
    if (m_fetchDone) {
        UiDiagnostics::Scope scope("HomeScreen::publishLibraryResult");
        finishFetch();
    }
    if (m_loadState == LoadState::Ready && m_resumeRefreshDone) {
        UiDiagnostics::Scope scope("HomeScreen::publishResumeResult");
        finishResumeRefresh();
    }
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
}



} // namespace miyoofin
