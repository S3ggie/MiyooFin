#include "HomeScreen.hpp"
#include "../../net/ServerAddress.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
#include "../MovieTitle.hpp"
#include "../ShowsBrowser.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include "../../download/DownloadSupport.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cctype>
#include <curl/curl.h>

namespace miyoofin {

// Layout constants
static constexpr int TAB_Y       = 0;
static constexpr int TAB_H       = 24;
static constexpr int INFO_Y      = 26;
static constexpr int INFO_H      = 110;
static constexpr int ROWS_Y      = INFO_Y + INFO_H + 2;
static constexpr int BOTTOM_H    = 18;
static constexpr int CARD_GAP    = 6;
static constexpr int ROW_STRIP_H = 96;  // max card height across all types
static constexpr int ROW_LABEL_H = 18;
static constexpr int VISIBLE_ROWS = 3;
static constexpr int SETTINGS_VISIBLE_ROWS = 6;
static constexpr int POSTER_MAX_CONCURRENT = 4;
static constexpr size_t POSTER_MAX_BYTES = 256 * 1024;
static constexpr int SEASON_POSTER_W = 74;
static constexpr int SEASON_POSTER_H = 111;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;
static constexpr int SHOWS_RAIL_W=36, SHOWS_PREVIEW_H=105, SHOWS_GRID_TOP=153;
static constexpr int SHOWS_HALF_W=302, SHOWS_LEFT_X=36, SHOWS_RIGHT_X=338;
static constexpr std::int64_t SYNC_FRESH_WALL_MS=15LL*60*1000;
static constexpr std::int64_t HIERARCHY_RECONCILE_MS=24LL*60*60*1000;
static std::int64_t wallClockMs(){return (std::int64_t)std::time(nullptr)*1000;}
// Keep the selected grid row within the compact three-row movie viewport.
// This is deliberately independent of logical MediaRow navigation.
static int clampMovieGridScrollCompact(int selected, int count, int currentScroll)
{
    if (count <= 0) return 0;
    selected = std::max(0, std::min(selected, count - 1));
    const int selectedRow = selected / MOVIE_GRID_COLUMNS;
    const int lastRow = (count - 1) / MOVIE_GRID_COLUMNS;
    const int maxScroll = std::max(0, lastRow - MOVIE_GRID_ROWS + 1);

    currentScroll = std::max(0, std::min(currentScroll, maxScroll));
    if (selectedRow < currentScroll) currentScroll = selectedRow;
    if (selectedRow >= currentScroll + MOVIE_GRID_ROWS)
        currentScroll = selectedRow - MOVIE_GRID_ROWS + 1;
    return std::max(0, std::min(currentScroll, maxScroll));
}

struct PosterTransfer { HomeScreen::PosterJob job; std::string url; std::vector<unsigned char> bytes; curl_slist *headers=nullptr; bool tooLarge=false; };
static size_t posterWrite(void *p, size_t s, size_t n, void *u) {
    PosterTransfer *t=static_cast<PosterTransfer*>(u); size_t z=s*n;
    if (z > POSTER_MAX_BYTES-t->bytes.size()) { t->tooLarge=true; return 0; }
    const unsigned char *b=static_cast<const unsigned char*>(p); t->bytes.insert(t->bytes.end(),b,b+z); return z;
}

// Width and height are now computed per-item via artworkBoxSize()

HomeScreen::HomeScreen(const Session &session, std::shared_ptr<DownloadManager> downloads)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_downloads(std::move(downloads))
    , m_userName(session.userName)
{
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
    { std::lock_guard<std::mutex> lock(m_hierarchyMutex); m_stopHierarchyWorker = true; }
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

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row)
{ return homeSettingsRowAction(row); }

std::vector<HomeScreen::SettingsAddressRow> HomeScreen::settingsAddressRows(const Session &session)
{ return homeSettingsAddressRows(session); }

int HomeScreen::settingsRowCount(const Session &session)
{ return homeSettingsRowCount(session); }

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row, const Session &session)
{ return homeSettingsRowAction(row, session); }

const char *HomeScreen::lastApiRouteValue()
{
    return RouteStatus::label(RouteStatus::latest());
}

std::vector<MediaItem> HomeScreen::combineMovieViews(const std::vector<CachedLibraryView> &views)
{ return miyoofin::combineMovieViews(views); }

void HomeScreen::refreshMovieFilter()
{
    const int movies=tabIndex("Movies"); if (movies < 0) return;
    std::vector<MediaItem> displayed;
    for (const auto &item : m_movieMaster) {
        if (movieMatchesAlphabetFilter(item.title, m_movieActiveLetter))
            displayed.push_back(item);
    }
    m_tabs[movies].rows = {{"Movies", std::move(displayed)}};
    m_activeRow = 0; m_activeCard = 0; m_rowScroll = 0; m_cardScroll = 0;
    m_selectedArtwork = {}; m_selectedArtworkId.clear(); m_selectedArtworkAttempted = false;
}

void HomeScreen::rebuildShowsPresentation() { ShowsPresentation p=makeShowsPresentation((presentationOffline()?m_offlineSnapshot:m_cachedSnapshot).shows); m_showMaster=std::move(p.shows); m_animeMaster=std::move(p.anime); refreshShowsFilter(); }
void HomeScreen::prepareOfflineProjection() { OfflineCatalogSnapshot catalog; const bool catalogLoaded=OfflineCatalog::load(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),catalog,nullptr); if(catalogLoaded){std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);m_catalogSnapshot=catalog;m_catalogSnapshotReady=true;} OfflineLibraryProjection p(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);m_fetchOfflineMovies=p.movies();m_fetchOfflineSnapshot=m_cachedSnapshot;for(auto &view:m_fetchOfflineSnapshot.shows){std::vector<MediaItem>filtered;for(const auto&i:view.items)if(p.playable(i.id)||!p.seasons(i.id).empty())filtered.push_back(i);view.items=std::move(filtered);}m_fetchOfflinePrepared=true; }
void HomeScreen::applyOfflineProjection() { const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;if(!m_fetchOfflinePrepared)return;m_tabs=std::move(m_fetchOfflineTabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_movieMaster=std::move(m_fetchOfflineMovies);m_offlineSnapshot=std::move(m_fetchOfflineSnapshot);m_fetchOfflinePrepared=false;refreshMovieFilter();rebuildShowsPresentation();clampNavigation(); }
void HomeScreen::applyPresentationProjection() {
    if (!m_haveCachedSnapshot) return;
    OfflineCatalogSnapshot catalog;
    { std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex); catalog=m_catalogSnapshot; }
    OfflineLibraryProjection projection(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
    m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);
    m_fetchOfflineMovies=projection.movies();
    m_fetchOfflineSnapshot=m_cachedSnapshot;
    for (auto &view : m_fetchOfflineSnapshot.shows) {
        std::vector<MediaItem> filtered;
        for (const auto &item : view.items) {
            if (projection.playable(item.id) || !projection.seasons(item.id).empty())
                filtered.push_back(item);
        }
        view.items=std::move(filtered);
    }
    m_fetchOfflinePrepared=true;
    applyOfflineProjection();
}
void HomeScreen::restoreOnlinePresentation() {
    if (!m_haveCachedSnapshot) return;
    const std::vector<TabData> previous=m_tabs; const int selected=m_activeTab;
    m_tabs=tabsFromSnapshot(m_cachedSnapshot);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
    m_movieMaster=combineMovieViews(m_cachedSnapshot.movies);
    refreshMovieFilter(); rebuildShowsPresentation(); clampNavigation();
}
void HomeScreen::refreshShowsFilter() { m_filteredShows.clear();m_filteredAnime.clear();for(const auto&i:m_showMaster)if(matchesAlphabetFilter(i.title,m_showsActiveLetter))m_filteredShows.push_back(i);for(const auto&i:m_animeMaster)if(matchesAlphabetFilter(i.title,m_showsActiveLetter))m_filteredAnime.push_back(i);m_showSelected=m_animeSelected=m_showScroll=m_animeScroll=0;m_showsFocus=!m_filteredShows.empty()?ShowsFocus::ShowsGrid:!m_filteredAnime.empty()?ShowsFocus::AnimeGrid:ShowsFocus::AlphabetRail;if(const MediaItem*i=showsSelectedItem())m_showsPreviewId=i->id; }
std::vector<MediaItem> HomeScreen::cachedSeasonsForSeries(const std::string &seriesId) const
{
    std::vector<MediaItem> seasons;
    OfflineCatalogSnapshot catalog;
    {
        std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
        if (!m_catalogSnapshotReady) return {};
        const auto seasonsIt=m_catalogSnapshot.seasonsBySeries.find(seriesId);
        if (seasonsIt==m_catalogSnapshot.seasonsBySeries.end()) return {};
        seasons=seasonsIt->second;
        if (presentationOffline()) {
            catalog.seasonsBySeries.emplace(seriesId,seasons);
            for (const auto &season:seasons) {
                const auto episodesIt=m_catalogSnapshot.episodesBySeason.find(season.id);
                if (episodesIt!=m_catalogSnapshot.episodesBySeason.end())
                    catalog.episodesBySeason.emplace(season.id,episodesIt->second);
            }
        }
    }
    if (presentationOffline()) {
        LibrarySnapshot library;
        OfflineLibraryProjection projection(library,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{});
        return projection.seasons(seriesId);
    }
    return seasons;
}
const MediaItem *HomeScreen::showsSelectedItem() const { const std::vector<MediaItem>*v=m_showsFocus==ShowsFocus::AnimeGrid?&m_filteredAnime:&m_filteredShows;int n=m_showsFocus==ShowsFocus::AnimeGrid?m_animeSelected:m_showSelected;if(n>=0&&n<(int)v->size())return &(*v)[n];for(const auto&i:m_filteredShows)if(i.id==m_showsPreviewId)return &i;for(const auto&i:m_filteredAnime)if(i.id==m_showsPreviewId)return &i;return nullptr; }
void HomeScreen::clampShowsNavigation() { if(!m_filteredShows.empty()){m_showSelected=std::max(0,std::min(m_showSelected,(int)m_filteredShows.size()-1));m_showScroll=clampShowsGridScroll(m_showSelected,m_filteredShows.size(),m_showScroll);}else m_showSelected=m_showScroll=0;if(!m_filteredAnime.empty()){m_animeSelected=std::max(0,std::min(m_animeSelected,(int)m_filteredAnime.size()-1));m_animeScroll=clampShowsGridScroll(m_animeSelected,m_filteredAnime.size(),m_animeScroll);}else m_animeSelected=m_animeScroll=0; }

int HomeScreen::moveMovieGridCompact(int index, int count, int deltaRow, int deltaCol) const
{
    if (count <= 0) return 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    int row = index / MOVIE_GRID_COLUMNS + deltaRow;
    int col = index % MOVIE_GRID_COLUMNS + deltaCol;
    if (col < 0 || col >= MOVIE_GRID_COLUMNS || row < 0) return index;
    int target = row * MOVIE_GRID_COLUMNS + col;
    if (target >= count) return deltaRow > 0 ? count - 1 : index;
    return target;
}

const TabData &HomeScreen::currentTab() const
{
    static const TabData empty{"", {}};
    return m_activeTab>=0 && m_activeTab<(int)m_tabs.size() ? m_tabs[m_activeTab] : empty;
}

const char *HomeScreen::diagnosticTabName() const { return currentTab().name.empty() ? "other" : currentTab().name.c_str(); }
bool HomeScreen::activeTabNamed(const char *name) const { return currentTab().name==name; }
int HomeScreen::tabIndex(const char *name) const { for(int i=0;i<(int)m_tabs.size();++i) if(m_tabs[i].name==name) return i; return -1; }

const MediaRow *HomeScreen::currentRow() const
{
    const auto &rows = currentTab().rows;
    if (m_activeRow < (int)rows.size())
        return &rows[m_activeRow];
    return nullptr;
}

const MediaItem *HomeScreen::currentItem() const
{
    const MediaRow *row = currentRow();
    if (row && m_activeCard < (int)row->items.size())
        return &row->items[m_activeCard];
    return nullptr;
}

void HomeScreen::clampNavigation()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        return;
    }
    if (activeTabNamed("Movies")) {
        // Movies has exactly one logical MediaRow.  Here m_rowScroll is the
        // first visible *grid* row, so never apply the generic row-list rules.
        m_activeRow = 0;
        const auto &items = rows[0].items;
        if (items.empty()) {
            m_activeCard = 0;
            m_rowScroll = 0;
            m_cardScroll = 0;
            return;
        }
        if (m_activeCard < 0) m_activeCard = 0;
        if (m_activeCard >= (int)items.size())
            m_activeCard = (int)items.size() - 1;
        m_rowScroll = clampMovieGridScrollCompact(
            m_activeCard, (int)items.size(), m_rowScroll);
        m_cardScroll = 0;
        return;
    }
    if (activeTabNamed("Shows")) { clampShowsNavigation(); return; }
    if (m_activeRow < 0) m_activeRow = 0;
    if (m_activeRow >= (int)rows.size()) m_activeRow = (int)rows.size() - 1;
    const auto &items = rows[m_activeRow].items;
    if (items.empty()) {
        m_activeCard = 0; m_cardScroll = 0;
        return;
    }
    if (m_activeCard < 0) m_activeCard = 0;
    if (m_activeCard >= (int)items.size()) m_activeCard = (int)items.size() - 1;
    if (m_activeRow < m_rowScroll) m_rowScroll = m_activeRow;
    if (m_activeRow >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_activeRow - VISIBLE_ROWS + 1;
    // Horizontal: m_cardScroll is a pixel offset, clamp so active card is visible
    static constexpr int HMARGIN = 4;
    m_cardScroll = clampCardScroll(items, m_activeCard, m_cardScroll,
                                    640, HMARGIN, CARD_GAP);
}

void HomeScreen::enter()
{
    printf("[HomeScreen] enter (tab=%d) user=%s\n", m_activeTab,
           m_userName.c_str());
    if (m_loadState == LoadState::Loading && !m_fetchDone) {
        std::string path = LibraryCache::cachePath("cache", LibraryCache::scopeKey(m_session.serverUrl, m_session.userId));
        bool cacheNeedsRefresh=false;
        if (LibraryCache::load(path, m_cachedSnapshot, nullptr, &cacheNeedsRefresh)) {
            m_tabs = tabsFromSnapshot(m_cachedSnapshot); m_haveCachedSnapshot = true;
            m_movieMaster = combineMovieViews(m_cachedSnapshot.movies);
            refreshMovieFilter();
            rebuildShowsPresentation();
            if (m_session.manualOfflineMode) applyPresentationProjection();
            m_loadState = LoadState::Ready; clampNavigation();
            printf("[HomeScreen] Loaded local library cache%s\n", cacheNeedsRefresh ? " (stale generation)" : "");
        }
        const std::string scope=LibraryCache::scopeKey(m_session.serverUrl,m_session.userId);
        const bool haveState=SyncStateStore::load(SyncStateStore::path("cache",scope),m_syncState);
        m_forceHierarchyReconcile=!haveState || !syncStateFresh(m_syncState,wallClockMs(),HIERARCHY_RECONCILE_MS);
        // Skip fetch ONLY when a valid current-generation snapshot exists and
        // SyncState confirms a recent successful sync.  An old-generation
        // snapshot (needsRefresh) or missing snapshot must always trigger a
        // fresh fetch, and a stale SyncState forces one regardless.
        if (m_haveCachedSnapshot && !cacheNeedsRefresh && haveState && syncStateFresh(m_syncState,wallClockMs(),SYNC_FRESH_WALL_MS)) {
            m_syncSchedule.hasSucceeded=true; m_syncSchedule.lastSuccess=SDL_GetTicks();
        } else requestFetch(SDL_GetTicks());
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

bool HomeScreen::handleAction(Action action)
{
    // Back always dismisses an armed logout prompt before any screen-specific
    // Back behavior (including the Movies alphabet rail).
    if (m_logoutArmed && action == Action::Back) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
        return true;
    }
    if (m_logoutArmed && action != Action::ActionsMenu) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
    }

    // Loading: only allow logout
    if (m_loadState == LoadState::Loading) {
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Error: allow retry (A) and logout (Y)
    if (m_loadState == LoadState::Error) {
        if (action == Action::Confirm) {
            m_loadState = LoadState::Loading;
            m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear();
            startFetch(); return true;
        }
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Ready: normal navigation
    if (activeTabNamed("Downloads") && handleDownloadsAction(action)) return true;
    if (activeTabNamed("Settings")) {
        if (action == Action::Back) {
            if (m_settingsConfirmation != SettingsConfirmation::None) {
                m_settingsConfirmation = SettingsConfirmation::None;
                return true;
            }
            return false;
        }
        if (action == Action::Up || action == Action::Down) {
            const int previous = m_settingsSelected;
            if (action == Action::Up && m_settingsSelected > 0) --m_settingsSelected;
            else if (action == Action::Down && m_settingsSelected < settingsRowCount(m_session)-1) ++m_settingsSelected;
            if (m_settingsSelected != previous)
                m_settingsConfirmation = SettingsConfirmation::None;
            m_settingsScroll=std::max(0,std::min(m_settingsSelected,settingsRowCount(m_session)-SETTINGS_VISIBLE_ROWS));
            return true;
        }
        if (action == Action::Confirm) {
            switch (settingsRowAction(m_settingsSelected,m_session)) {
            case SettingsRowAction::OfflineMode:
                m_session.manualOfflineMode = !m_session.manualOfflineMode;
                m_session.save();
                if (m_session.manualOfflineMode) applyPresentationProjection();
                else if (!m_libraryOffline) restoreOnlinePresentation();
                return true;
            case SettingsRowAction::LocalAddress:
                m_localAddressRequested = true;
                return true;
            case SettingsRowAction::PublicAddress:
                m_publicAddressRequested = true;
                return true;
            case SettingsRowAction::ChangeServer:
            case SettingsRowAction::Logout: {
                const SettingsConfirmation requested = settingsRowAction(m_settingsSelected,m_session) == SettingsRowAction::ChangeServer
                    ? SettingsConfirmation::ChangeServer : SettingsConfirmation::Logout;
                if (m_settingsConfirmation == requested) {
                    if (requested == SettingsConfirmation::ChangeServer)
                        m_changeServerRequested = true;
                    else
                        m_logoutRequested = true;
                    m_settingsConfirmation = SettingsConfirmation::None;
                } else {
                    m_settingsConfirmation = requested;
                }
                return true;
            }
            case SettingsRowAction::None:
                break;
            }
        }
        // Settings owns its account actions; do not let Y or X trigger Home actions.
        if (action == Action::ActionsMenu || action == Action::Search) return true;
        if (action != Action::NextTab && action != Action::PrevTab) return true;
        m_settingsScroll=std::max(0,std::min(m_settingsSelected,settingsRowCount(m_session)-SETTINGS_VISIBLE_ROWS));
    }
    switch (action) {
    case Action::Up:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(m_showsAlphabetFocus>0)--m_showsAlphabetFocus;}else if(m_showsFocus==ShowsFocus::ShowsGrid)m_showSelected=moveShowsGrid(m_showSelected,m_filteredShows.size(),-1,0);else m_animeSelected=moveShowsGrid(m_animeSelected,m_filteredAnime.size(),-1,0);clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) { if (m_movieAlphabetFocus > 0) --m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),-1,0); }
        else m_activeRow--;
        clampNavigation(); return true;
    case Action::Down:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(m_showsAlphabetFocus<25)++m_showsAlphabetFocus;}else if(m_showsFocus==ShowsFocus::ShowsGrid)m_showSelected=moveShowsGrid(m_showSelected,m_filteredShows.size(),1,0);else m_animeSelected=moveShowsGrid(m_animeSelected,m_filteredAnime.size(),1,0);clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) { if (m_movieAlphabetFocus < 25) ++m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),1,0); }
        else m_activeRow++;
        clampNavigation(); return true;
    case Action::Left:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail)return true;if(m_showsFocus==ShowsFocus::ShowsGrid){if(m_showSelected%4)m_showSelected--;else{m_showsFocus=ShowsFocus::AlphabetRail;m_showsAlphabetFocus=m_showsActiveLetter>=0?m_showsActiveLetter:alphabetFocus(m_filteredShows[m_showSelected].title);}}else{if(m_animeSelected%4)m_animeSelected--;else if(!m_filteredShows.empty()){m_showsFocus=ShowsFocus::ShowsGrid;m_showSelected=crossShowsGridIndex(m_animeSelected,m_filteredShows.size(),false);}else m_showsFocus=ShowsFocus::AlphabetRail;}clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) {
            if (!m_movieRailFocused && (!currentRow() || m_activeCard % MOVIE_GRID_COLUMNS == 0)) {
                m_movieRailFocused = true;
                m_movieAlphabetFocus = m_movieActiveLetter >= 0
                    ? m_movieActiveLetter
                    : movieAlphabetFocus(currentItem() ? currentItem()->title : std::string());
            } else if (!m_movieRailFocused && currentRow()) {
                m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),0,-1);
            }
        }
        else m_activeCard--;
        clampNavigation(); return true;
    case Action::Right:
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){if(!m_filteredShows.empty())m_showsFocus=ShowsFocus::ShowsGrid;else if(!m_filteredAnime.empty())m_showsFocus=ShowsFocus::AnimeGrid;}else if(m_showsFocus==ShowsFocus::ShowsGrid){if(m_showSelected%4<3)m_showSelected++;else if(!m_filteredAnime.empty()){m_showsFocus=ShowsFocus::AnimeGrid;m_animeSelected=crossShowsGridIndex(m_showSelected,m_filteredAnime.size(),true);}}else if(m_animeSelected%4<3)m_animeSelected++;clampShowsNavigation();return true;}
        if (activeTabNamed("Movies")) { if (m_movieRailFocused) m_movieRailFocused=false; else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),0,1); }
        else m_activeCard++;
        clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if((activeTabNamed("Downloads")||activeTabNamed("Settings"))&&m_downloads) { if(activeTabNamed("Downloads"))m_downloads->requestReconcile(); refreshDownloads(); } return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if((activeTabNamed("Downloads")||activeTabNamed("Settings"))&&m_downloads) { if(activeTabNamed("Downloads"))m_downloads->requestReconcile(); refreshDownloads(); } return true;
    case Action::Search: return false;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){m_showsActiveLetter=m_showsActiveLetter==m_showsAlphabetFocus?-1:m_showsAlphabetFocus;refreshShowsFilter();return true;}if(const MediaItem*i=showsSelectedItem()){m_stack->push(std::make_unique<SeriesScreen>(m_session,*i,m_downloads,m_libraryOffline,cachedSeasonsForSeries(i->id),presentationOffline()));return true;}return true;}
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieActiveLetter = m_movieActiveLetter == m_movieAlphabetFocus ? -1 : m_movieAlphabetFocus;
            refreshMovieFilter();
            return true;
        }
        if(activeTabNamed("Shows")&&m_showsFocus==ShowsFocus::AlphabetRail){m_showsFocus=!m_filteredShows.empty()?ShowsFocus::ShowsGrid:!m_filteredAnime.empty()?ShowsFocus::AnimeGrid:ShowsFocus::AlphabetRail;return true;}
        const MediaItem *item = currentItem();
        if (item) {
            printf("[HomeScreen] Select: %s (%s)\n",
                   item->title.c_str(), item->type.c_str());
            if (item->type == "show") {
                m_stack->push(std::make_unique<SeriesScreen>(m_session, *item,m_downloads,m_libraryOffline,cachedSeasonsForSeries(item->id),presentationOffline()));
                return true;
            }
            if (item->type == "movie") {
                UiDiagnostics::Scope openScope("HomeScreen::open MovieDetailsScreen");
                std::unique_ptr<Screen> movieScreen;
                {
                    // This outer construction scope includes Session/MediaItem
                    // copies performed before the constructor body starts.
                    UiDiagnostics::Scope constructionScope("MovieDetailsScreen::construction");
                    std::shared_ptr<const DecodedImage> gridArtwork;
                    const auto artwork=m_rowArtwork.find(rowArtworkKey(*item));
                    if(artwork!=m_rowArtwork.end() && artwork->second.status==RowArtworkStatus::Loaded
                        && artwork->second.image && !artwork->second.image->empty())
                        gridArtwork=artwork->second.image;
                    movieScreen=std::make_unique<MovieDetailsScreen>(m_session,*item,m_downloads,std::move(gridArtwork));
                }
                m_stack->push(std::move(movieScreen));
                return true;
            }
            if (item->type == "episode") {
                // B5e3b: Open EpisodeBrowserScreen focused on this episode
                if (!item->seriesId.empty() && !item->seasonId.empty()) {
                    MediaItem series;
                    series.id = item->seriesId;
                    series.title = item->seriesName;
                    series.type = "show";

                    MediaItem season;
                    season.id = item->seasonId;
                    season.type = "season";
                    season.indexNumber = item->parentIndexNumber;
                    if (item->parentIndexNumber > 0) {
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), "Season %d",
                                      item->parentIndexNumber);
                        season.title = buf;
                    } else {
                        season.title = "Season";
                    }

                    m_stack->push(std::make_unique<EpisodeBrowserScreen>(
                        m_session, series, season, item->id,m_downloads,m_libraryOffline,presentationOffline()));
                    return true;
                }
                printf("[HomeScreen] Cannot open episode browser: "
                       "missing series/season context\n");
            }
        }
        return true;
    }
    case Action::Back:
        if (m_logoutArmed) { m_logoutArmed = false; m_logoutTimer = 0; return true; }
        if (activeTabNamed("Movies") && m_movieRailFocused) {
            m_movieRailFocused = false;
            return true;
        }
        return false;
    default: return false;
    }
}

void HomeScreen::startDownloadRefresh()
{
    if (!m_downloads) { m_downloadSnapshot = {}; return; }
    if (m_downloadRefreshInFlight) return;
    if (m_downloadRefreshThread.joinable()) m_downloadRefreshThread.join();
    m_downloadRefreshDone=false;m_downloadRefreshInFlight=true;
    std::shared_ptr<DownloadManager> downloads=m_downloads;
    const std::string journalPath=OfflinePlaybackJournal::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    const bool valid=m_session.valid();
    m_downloadRefreshThread=std::thread([this,downloads,journalPath,valid] {
        DownloadSnapshot snapshot=downloads->snapshot();
        std::vector<OfflinePlaybackEntry> missing;
        if(valid) {
            std::vector<OfflinePlaybackEntry> journal;
            if(OfflinePlaybackJournal::load(journalPath,journal,nullptr))
                for(const auto &entry:journal)if(entry.serverMissing&&!entry.conflict)missing.push_back(entry);
        }
        m_downloadRefreshResult=std::move(snapshot);
        m_downloadJournalResult=std::move(missing);
        m_downloadRefreshDone=true;
    });
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


void HomeScreen::startFetch()
{
    // A single worker owns all Movies/Shows networking.  Tab flips merely
    // coalesce while it is active.
    if (m_fetchThread.joinable()) return;
    m_fetchDone = false;
    m_fetchError.clear();
    m_fetchResult.clear();
    m_fetchCacheSaved = false;
    m_fetchOfflinePrepared = false;

    Session session=m_session;
    std::string url   = session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid   = m_session.userId;
    std::string devId = m_session.deviceId;

    m_fetchThread = std::thread([this, session, url, token, uid, devId]() {
        std::string err;
        auto fail=[&](const std::string &error) {
            m_fetchError=error;
            if(m_haveCachedSnapshot)prepareOfflineProjection();
            m_fetchDone=true;
        };

        // 1. Get library views
        std::vector<LibraryView> views;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getViews(base, token, uid, devId, views, err);},err)) {
            fail(err);
            return;
        }
        printf("[HomeScreen] Got %zu library views\n", views.size());

        // 2. Continue watching (non-fatal if fails)
        std::vector<MediaItem> cw;
        std::string cwErr;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12, cw, cwErr);},cwErr))
            printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());

        // 3. Recently added (non-fatal)
        std::vector<MediaItem> ra;
        std::string raErr;
        if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLatestItems(base, token, uid, devId, 16, ra, raErr);},raErr))
            printf("[HomeScreen] Recently added: %s\n", raErr.c_str());

        // 4. Per-library items
        std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
        std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
        LibrarySnapshot snapshot;
        snapshot.continueWatching = cw;
        snapshot.recentlyAdded = ra;
        for (const auto &v : views) {
            if (v.collectionType == "movies") {
                std::vector<MediaItem> items; std::string ie;
                if (RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLibraryItems(base, token, uid, devId,v.id, "Movie", 50, items, ie);},ie)) {
                    moviesByView.push_back({v.name, std::move(items)});
                    snapshot.movies.push_back({v.id, v.name, v.collectionType, moviesByView.back().second});
                } else { fail(ie); return; }
            } else if (v.collectionType == "tvshows") {
                std::vector<MediaItem> items; std::string ie;
                if (RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getLibraryItems(base, token, uid, devId,v.id, "Series", 50, items, ie);},ie)) {
                    showsByView.push_back({v.name, std::move(items)});
                    snapshot.shows.push_back({v.id, v.name, v.collectionType, showsByView.back().second});
                } else { fail(ie); return; }
            }
        }

        m_fetchResult = JellyfinApi::buildTabs(views, cw, ra, moviesByView, showsByView);
        m_remoteSnapshot = std::move(snapshot);
        std::set<std::string> changedSeries;
        // A top-level listing is cheap and catches added/deleted series.  The
        // change feed catches episode/season UserData and metadata edits
        // without walking every cached hierarchy.
        if (m_syncState.lastSuccessfulMs > 0 && !m_forceHierarchyReconcile) {
            std::vector<MediaItem> changed; std::string changedError;
            if (!RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getChangedHierarchyItems(base,token,uid,devId,m_syncState.lastSuccessfulMs,changed,changedError);},changedError)) {
                fail(changedError); return;
            }
            for(const auto&i:changed) {
                if(i.type=="show") changedSeries.insert(i.id);
                else if(!i.seriesId.empty()) changedSeries.insert(i.seriesId);
            }
        }
        // Disk writes, poster enumeration and catalog scheduling used to run
        // from finishFetch() on SDL's update path.  Do all of that here.
        std::vector<StalePoster> stale;
        m_fetchStats=LibraryCache::reconcile(m_cachedSnapshot,m_remoteSnapshot,&stale);
        const std::string scope=LibraryCache::scopeKey(url,uid);
        if (LibraryCache::save(LibraryCache::cachePath("cache",scope),m_remoteSnapshot)) {
            m_fetchCacheSaved=true;
            for(const auto&p:stale) ImageCache::removeCached(p.itemId,ImageType::Primary,p.tag,64,96);
            startPosterSync(m_remoteSnapshot);
            startHierarchyCache(m_remoteSnapshot,m_cachedSnapshot,changedSeries);
        }
        /* Poster work is deliberately not part of metadata completion.
        // sync worker; UI selection/rendering never issues these requests.
        std::vector<MediaItem> posterJobs;
        for (const auto &v : m_remoteSnapshot.movies) for (const auto &i : v.items) {
            auto p=i.imageTags.find("Primary");
            if (p!=i.imageTags.end() && !p->second.empty() && !ImageCache::isCached(i.id,ImageType::Primary,p->second,64,96)) posterJobs.push_back(i);
        }
        for (const auto &v : m_remoteSnapshot.shows) for (const auto &i : v.items) {
            auto p=i.imageTags.find("Primary");
            if (p!=i.imageTags.end() && !p->second.empty() && !ImageCache::isCached(i.id,ImageType::Primary,p->second,64,96)) posterJobs.push_back(i);
        }
        std::atomic<size_t> next{0}; std::vector<std::thread> downloaders;
        const int workers = posterJobs.size() < 4 ? (int)posterJobs.size() : 4;
        for (int w=0; w<workers; ++w) downloaders.emplace_back([&] {
            for (;;) { size_t n=next.fetch_add(1); if (n>=posterJobs.size()) break; const MediaItem &i=posterJobs[n];
                auto p=i.imageTags.find("Primary"); HttpClient client; client.setTimeoutSec(8);
                BinaryHttpResponse response; std::string pe;
                if (client.getBinary(buildImageUrl(url,i.id,ImageType::Primary,p->second,64,96), JellyfinApi::buildAuthHeaders(token,devId),response,pe,256*1024)
                    && response.ok() && !response.data.empty()) ImageCache::writeToCache(i.id,ImageType::Primary,p->second,64,96,response.data.data(),response.data.size());
            }
        });
        for (auto &worker : downloaders) worker.join();
        */ m_fetchDone = true;
    });
}

void HomeScreen::requestFetch(Uint32 now)
{
    if (m_syncSchedule.request(now)) startFetch();
}

void HomeScreen::finishFetch()
{
    if (m_fetchThread.joinable()) m_fetchThread.join();
    m_fetchDone = false;
    if (!m_fetchError.empty()) {
        // Cached libraries remain usable when DNS/network access is transiently
        // unavailable.  The schedule's retry delay prevents tab flips from
        // turning that failure into a request storm.
        m_libraryOffline = m_haveCachedSnapshot;
        if (m_libraryOffline) applyOfflineProjection();
        if (!m_haveCachedSnapshot) m_loadState = LoadState::Error;
        printf("[HomeScreen] Fetch failed: %s\n", m_fetchError.c_str());
        m_syncSchedule.complete(SDL_GetTicks(), false);
        return;
    }
    if (!m_fetchCacheSaved) {
        printf("[HomeScreen] Library cache save failed; retaining old cache\n");
    } else { m_cachedSnapshot = m_remoteSnapshot; m_haveCachedSnapshot = true; }
    const std::vector<TabData> previous=m_tabs;
    const int selected=m_activeTab;
    m_tabs = std::move(m_fetchResult);
    m_activeTab=transitionTabIndex(previous,selected,m_tabs);
    m_libraryOffline = false;
    m_movieMaster = combineMovieViews(m_cachedSnapshot.movies);
    refreshMovieFilter();
    rebuildShowsPresentation();
    if (m_session.manualOfflineMode) applyPresentationProjection();
    m_loadState = LoadState::Ready;
    clampNavigation();
    printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n", m_tabs.size(), m_fetchStats.added, m_fetchStats.changed);
    m_syncSchedule.complete(SDL_GetTicks(), true);
}

void HomeScreen::startPosterSync(const LibrarySnapshot &snapshot)
{
    queuePosterJobs(collectPosterJobs(snapshot));
}

void HomeScreen::queuePosterJobs(std::vector<PosterJob> jobs)
{
    std::lock_guard<std::mutex> lock(m_posterMutex);
    std::set<std::string> queued;
    for (const auto &job : m_pendingPosterJobs)
        queued.insert(job.itemId + ":" + job.imageTag + ":" + std::to_string(job.width) + "x" + std::to_string(job.height));
    for (auto &job : jobs) {
        std::string key=job.itemId + ":" + job.imageTag + ":" + std::to_string(job.width) + "x" + std::to_string(job.height);
        if (queued.insert(key).second && !ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height))
            m_pendingPosterJobs.push_back(std::move(job));
    }
    m_posterWake.notify_one();
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectPosterJobs(const LibrarySnapshot &snapshot)
{
    std::vector<PosterJob> out;
    for (auto &job : planHomePosterJobs(snapshot))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) out.push_back(std::move(job));
    return out;
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectSeasonPosterJobs(const std::vector<MediaItem> &seasons)
{
    std::vector<PosterJob> out;
    for (auto &job : planSeasonPosterJobs(seasons))
        if (!ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) out.push_back(std::move(job));
    return out;
}

void HomeScreen::startHierarchyCache(const LibrarySnapshot &snapshot, const LibrarySnapshot &previous,
                                     const std::set<std::string> &changedSeries)
{
    std::vector<MediaItem> all, shows; std::set<std::string> seen;
    for (const auto &view : snapshot.shows) for (const auto &show : view.items)
        if (!show.id.empty() && seen.insert(show.id).second) all.push_back(show);
    OfflineCatalogSnapshot catalogSnapshot;
    const std::string catalog=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    const bool catalogValid=OfflineCatalog::load(catalog,catalogSnapshot);
    std::map<std::string,MediaItem> old;
    for(const auto&v:previous.shows)for(const auto&i:v.items)old[i.id]=i;
    for(const auto&s:all){auto it=old.find(s.id);if(!catalogValid||m_forceHierarchyReconcile||changedSeries.count(s.id)||it==old.end()||!LibraryCache::itemEquivalent(it->second,s))shows.push_back(s);}
    // An authoritative top-level list also provides deletion reconciliation;
    // this remains background work and never affects download files.
    OfflineCatalog::reconcileSeries(catalog,all,nullptr);
    // This function runs from the library fetch worker.  Refresh the reusable
    // RAM catalog after reconciliation, never from Home's SDL-thread push.
    if (OfflineCatalog::load(catalog,catalogSnapshot,nullptr)) {
        std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
        m_catalogSnapshot=std::move(catalogSnapshot);
        m_catalogSnapshotReady=true;
    }
    std::lock_guard<std::mutex> lock(m_hierarchyMutex);
    const std::uint64_t generation=m_hierarchyGeneration.fetch_add(1)+1;
    m_pendingHierarchyShows=std::move(shows); // a newer library snapshot supersedes queued work
    m_pendingHierarchyGeneration=generation;
    m_hierarchyCompleted.store(0);
    m_hierarchyTotal.store(m_pendingHierarchyShows.size());
    m_hierarchyActive.store(!m_pendingHierarchyShows.empty());
    if(m_pendingHierarchyShows.empty()) {
        m_syncState.lastSuccessfulMs=wallClockMs();
        if(m_forceHierarchyReconcile)m_syncState.lastReconcileMs=m_syncState.lastSuccessfulMs;
        SyncStateStore::save(SyncStateStore::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_syncState);
    }
    m_hierarchyOffline.store(false);
    m_hierarchyWake.notify_one();
}

void HomeScreen::hierarchyWorker()
{
    for (;;) {
        std::vector<MediaItem> shows;
        std::uint64_t generation=0;
        { std::unique_lock<std::mutex> lock(m_hierarchyMutex); m_hierarchyWake.wait(lock,[&]{return m_stopHierarchyWorker||!m_pendingHierarchyShows.empty();}); if(m_stopHierarchyWorker)return; shows.swap(m_pendingHierarchyShows); generation=m_pendingHierarchyGeneration; }
        const std::string catalog=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
        for (const auto &series : shows) {
            { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker)return; }
            std::vector<MediaItem> seasons; std::string error;
            if (!RouteRequest(m_session).run([&](const std::string &base){return JellyfinApi::getSeasons(base,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,seasons,error);},error)) { if(generation==m_hierarchyGeneration.load()) m_hierarchyOffline.store(true); continue; }
            queuePosterJobs(collectSeasonPosterJobs(seasons));
            std::map<std::string,std::vector<MediaItem> > episodesBySeason;
            bool complete=true;
            for (const auto &season : seasons) {
                { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker)return; }
                if (season.id.empty()) { complete=false; break; }
                std::vector<MediaItem> episodes; error.clear();
                if (!RouteRequest(m_session).run([&](const std::string &base){return JellyfinApi::getEpisodes(base,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,season.id,episodes,error);},error)) { complete=false; if(generation==m_hierarchyGeneration.load()) m_hierarchyOffline.store(true); break; }
                episodesBySeason[season.id]=std::move(episodes);
            }
            // A show is only complete after every discovered level has been
            // fetched and atomically merged into the offline catalog.
            if (complete && OfflineCatalog::storeDiscoveredHierarchy(catalog,series,seasons,episodesBySeason,true,nullptr)) {
                // Reload on this background worker so the RAM snapshot exactly
                // follows catalog merge semantics and is ready for handoff.
                OfflineCatalogSnapshot snapshot;
                if (OfflineCatalog::load(catalog,snapshot,nullptr)) {
                    std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);
                    m_catalogSnapshot=std::move(snapshot);
                    m_catalogSnapshotReady=true;
                }
                if (generation==m_hierarchyGeneration.load()) m_hierarchyCompleted.fetch_add(1);
            }
        }
        if (generation==m_hierarchyGeneration.load()) {
            m_hierarchyActive.store(false);
            // A watermark means the requested hierarchy was fully committed,
            // never merely that the metadata request happened.  Failures keep
            // the old checkpoint so the next online attempt is conservative.
            if (!m_hierarchyOffline.load() && m_hierarchyCompleted.load()==m_hierarchyTotal.load()) {
                m_syncState.lastSuccessfulMs=wallClockMs();
                if(m_forceHierarchyReconcile)m_syncState.lastReconcileMs=m_syncState.lastSuccessfulMs;
                SyncStateStore::save(SyncStateStore::path("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_syncState);
                m_forceHierarchyReconcile=false;
            }
        }
    }
}

void HomeScreen::posterWorker()
{
    for (;;) {
        std::vector<PosterJob> jobs;
        { std::unique_lock<std::mutex> lock(m_posterMutex); m_posterWake.wait(lock,[&]{return m_stopPosterWorker||!m_pendingPosterJobs.empty();}); if(m_stopPosterWorker) return; jobs.swap(m_pendingPosterJobs); }
        for(const auto &job:jobs){ HttpClient client;client.setTimeoutSec(8);BinaryHttpResponse response;std::string error; if(RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,job.itemId,job.imageType,job.imageTag,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),response,error,512*1024)&&response.ok();},error)&&!response.data.empty())ImageCache::writeToCache(job.itemId,job.imageType,job.imageTag,job.width,job.height,response.data.data(),response.data.size()); }
    }
}

void HomeScreen::startResumeRefresh()
{
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();

    m_resumeRefreshDone = false;
    m_resumeRefreshInFlight = true;
    m_resumeRefreshSucceeded = false;
    m_resumeRefreshCacheSaved = false;
    m_resumeRefreshError.clear();
    m_resumeRefreshResult.clear();

    Session session=m_session;
    std::string url = session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    LibrarySnapshot snapshot=m_cachedSnapshot;
    const std::string cachePath=LibraryCache::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    m_resumeRefreshThread = std::thread([this, session, url, token, uid, devId, snapshot, cachePath]() mutable {
        std::vector<MediaItem> items;
        std::string error;
        if (RouteRequest(session).run([&](const std::string &base){return JellyfinApi::getResumeItems(base, token, uid, devId, 12,items, error);},error)) {
            m_resumeRefreshResult = std::move(items);
            m_resumeRefreshSucceeded = true;
            snapshot.continueWatching=m_resumeRefreshResult;
            m_resumeRefreshCacheSaved=LibraryCache::save(cachePath,snapshot);
            startPosterSync(snapshot);
        } else {
            m_resumeRefreshError = error;
        }
        m_resumeRefreshDone = true;
    });
}

void HomeScreen::finishResumeRefresh()
{
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();
    m_resumeRefreshDone = false;
    m_resumeRefreshInFlight = false;

    if (!m_resumeRefreshSucceeded) {
        printf("[HomeScreen] Continue Watching refresh failed: %s\n",
               m_resumeRefreshError.c_str());
    } else {
        updateContinueWatchingRow(m_tabs, m_resumeRefreshResult);
        m_cachedSnapshot.continueWatching = m_resumeRefreshResult;
        if (!m_resumeRefreshCacheSaved)
            printf("[HomeScreen] Continue Watching cache save failed\n");
        clampNavigation();
        printf("[HomeScreen] Continue Watching refreshed: %zu items\n",
               m_resumeRefreshResult.size());
    }

    if (m_resumeRefreshPending) {
        m_resumeRefreshPending = false;
        startResumeRefresh();
    }
}

void HomeScreen::tryLoadSelectedArtwork()
{
    const MediaItem *item = activeTabNamed("Shows") ? showsSelectedItem() : currentItem();
    if (!item) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    DisplayArtwork artwork = displayArtworkForItem(*item);
    if (!artwork.valid()) {
        // No Primary tag — clear artwork, keep placeholder
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    std::string key = rowArtworkKey(*item);

    // Shows artwork is always decoded by the background worker.  The selected
    // key is first in its working set, and the preview reads the same RAM
    // image as the grid card once it arrives.
    if (activeTabNamed("Shows")) {
        if (m_selectedArtworkId != key) m_selectedArtwork = {};
        m_selectedArtworkId = key;
        m_selectedArtworkAttempted = true;
        return;
    }

    // Already attempted this exact selection?  Do not retry.
    if (m_selectedArtworkAttempted && m_selectedArtworkId == key)
        return;

    // New selection — reset and attempt once
    m_selectedArtwork = {};
    m_selectedArtworkId = key;
    // Local library artwork remains retryable until the poster worker writes
    // it; Home retains its historical one-shot network behavior below.
    m_selectedArtworkAttempted = false;

    // Cache probing, reads and JPEG decode run on the existing bounded decode
    // worker.  A missing poster remains retryable after poster sync completes.
    submitDecode(*item,true,false);
}

// -------------------------------------------------------------------
// B5d2a: Row card artwork — loading state only (no rendering)
// -------------------------------------------------------------------

std::string HomeScreen::rowArtworkKey(const MediaItem &item)
{
    return homeArtworkKey(item);
}

void HomeScreen::evictRowArtworkIfNeeded()
{
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    while ((int)m_rowArtworkOrder.size() > ROW_ARTWORK_RAM_LIMIT) {
        auto victim = std::find_if(m_rowArtworkOrder.begin(),
                                   m_rowArtworkOrder.end(),
            [&](const std::string &key) {
                return protectedKeys.find(key) == protectedKeys.end();
            });
        // A temporary overflow is preferable to evicting an image being
        // rendered.  This is only possible when every cached key is visible.
        if (victim == m_rowArtworkOrder.end()) break;
        m_rowArtwork.erase(*victim);
        m_rowArtworkOrder.erase(victim);
    }
}

void HomeScreen::touchRowArtwork(const std::string &key)
{
    auto it = std::find(m_rowArtworkOrder.begin(), m_rowArtworkOrder.end(), key);
    if (it != m_rowArtworkOrder.end()) m_rowArtworkOrder.erase(it);
    m_rowArtworkOrder.push_back(key);
}

void HomeScreen::storeDecodedRowArtwork(const std::string &key, DecodedImage image)
{
    RowArtworkEntry &entry = m_rowArtwork[key];
    entry.status = RowArtworkStatus::Loaded;
    entry.image = std::make_shared<DecodedImage>(std::move(image));
    touchRowArtwork(key);
    evictRowArtworkIfNeeded();
}

void HomeScreen::submitDecode(const MediaItem &item, bool highPriority, bool shows)
{
    std::string key=rowArtworkKey(item); DisplayArtwork a=displayArtworkForItem(item);
    if(key.empty() || !a.valid()) return;
    std::lock_guard<std::mutex> lock(m_decodeMutex);
    if(!m_decodeOutstanding.insert(key).second) return;
    if(m_decodeJobs.size() >= 32) { m_decodeOutstanding.erase(key); return; }
    DecodeJob job{key,{item.id,a.imageType,a.tag,a.width,a.height},shows};
    if(highPriority) m_decodeJobs.push_front(std::move(job)); else m_decodeJobs.push_back(std::move(job));
    m_decodeWake.notify_one();
}

void HomeScreen::decodeWorker()
{
    for (;;) { DecodeJob job; { std::unique_lock<std::mutex> lock(m_decodeMutex); m_decodeWake.wait(lock,[&]{return m_stopDecodeWorker||!m_decodeJobs.empty();}); if(m_stopDecodeWorker) return; job=std::move(m_decodeJobs.front());m_decodeJobs.pop_front(); }
        auto bytes=ImageCache::readCached(job.artwork.itemId,job.artwork.imageType,job.artwork.imageTag,job.artwork.width,job.artwork.height);
        DecodedImage image=bytes.empty()?DecodedImage{}:ImageDecoder::decodeJpeg(bytes.data(),bytes.size());
        std::lock_guard<std::mutex> lock(m_decodeMutex); m_decodeResults.push_back({std::move(job.key),std::move(image),job.shows,!bytes.empty()});
    }
}

void HomeScreen::drainDecodedArtwork()
{
    std::deque<DecodeResult> results;
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        results.swap(m_decodeResults);
        for (const auto &result : results) m_decodeOutstanding.erase(result.key);
    }
    const std::set<std::string> protectedKeys = protectedRowArtworkKeys();
    for (auto &result : results) {
        // A job already being decoded cannot be cancelled.  Its result is not
        // allowed to displace current Shows artwork after a scroll, though.
        if (result.shows
            && m_activeShowsDecodeKeys.find(result.key) == m_activeShowsDecodeKeys.end()
            && protectedKeys.find(result.key) == protectedKeys.end()) continue;
        if (!result.cachePresent) {
            // Poster sync may populate this key later; do not make a cache miss
            // a permanent failure.
            continue;
        }
        if (result.image.empty()) {
            m_rowArtwork[result.key].status = RowArtworkStatus::Failed;
        } else {
            if(result.key==m_selectedArtworkId) {
                m_selectedArtwork=result.image;
                m_selectedArtworkAttempted=true;
            }
            storeDecodedRowArtwork(result.key, std::move(result.image));
        }
    }
}

std::set<std::string> HomeScreen::protectedRowArtworkKeys() const
{
    std::set<std::string> keys;
    auto add = [&](const MediaItem &item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty()) keys.insert(key);
    };
    auto addGrid = [&](const std::vector<MediaItem> &items, int scroll,
                       int columns, int rows) {
        const int first = std::max(0, scroll) * columns;
        const int last = std::min((int)items.size(), first + columns * rows);
        for (int i = first; i < last; ++i) add(items[i]);
    };

    if (activeTabNamed("Shows")) {
        addGrid(m_filteredShows, m_showScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        addGrid(m_filteredAnime, m_animeScroll, SHOWS_GRID_COLUMNS, SHOWS_GRID_ROWS);
        if (const MediaItem *item = showsSelectedItem()) add(*item);
        return keys;
    }
    if (activeTabNamed("Movies")) {
        const int movies=tabIndex("Movies");
        if (movies >= 0 && !m_tabs[movies].rows.empty()) {
            const auto &items = m_tabs[movies].rows[0].items;
            addGrid(items, m_rowScroll, MOVIE_GRID_COLUMNS, MOVIE_GRID_ROWS);
            if (const MediaItem *item = currentItem()) add(*item);
        }
        return keys;
    }

    // Home's viewport is horizontal and each row has variable card widths.
    const auto &rows = currentTab().rows;
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        const int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        int cardX = 4;
        for (const auto &item : rows[rowIdx].items) {
            const ArtworkBox box = artworkBoxSize(item);
            const int screenX = cardX - m_cardScroll;
            if (screenX + box.w >= 4 && screenX <= 636) add(item);
            if (screenX > 636) break;
            cardX += box.w + CARD_GAP;
        }
    }
    if (const MediaItem *item = currentItem()) add(*item);
    return keys;
}

void HomeScreen::updateShowsDecodeWorkingSet()
{
    std::vector<const MediaItem *> desired;
    std::set<std::string> keys;
    if (!activeTabNamed("Shows")) {
        m_activeShowsDecodeKeys.clear();
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (auto it = m_decodeJobs.begin(); it != m_decodeJobs.end();) {
            if (it->shows) {
                m_decodeOutstanding.erase(it->key);
                it = m_decodeJobs.erase(it);
            } else ++it;
        }
        return;
    }
    auto add = [&](const MediaItem &item) {
        const std::string key = rowArtworkKey(item);
        if (!key.empty() && keys.insert(key).second) desired.push_back(&item);
    };
    if (const MediaItem *selected = showsSelectedItem()) add(*selected);
    auto addGrid = [&](const std::vector<MediaItem> &items, int scroll) {
        const int first = std::max(0, scroll) * SHOWS_GRID_COLUMNS;
        const int last = std::min((int)items.size(), first + SHOWS_GRID_COLUMNS * SHOWS_GRID_ROWS);
        for (int i = first; i < last; ++i) add(items[i]);
    };
    if (m_showsFocus == ShowsFocus::AnimeGrid) {
        addGrid(m_filteredAnime, m_animeScroll); addGrid(m_filteredShows, m_showScroll);
    } else {
        addGrid(m_filteredShows, m_showScroll); addGrid(m_filteredAnime, m_animeScroll);
    }
    m_activeShowsDecodeKeys.swap(keys);
    {
        std::lock_guard<std::mutex> lock(m_decodeMutex);
        for (auto it = m_decodeJobs.begin(); it != m_decodeJobs.end();) {
            if (it->shows && m_activeShowsDecodeKeys.find(it->key) == m_activeShowsDecodeKeys.end()) {
                m_decodeOutstanding.erase(it->key);
                it = m_decodeJobs.erase(it);
            } else ++it;
        }
    }
    for (size_t i = 0; i < desired.size(); ++i) {
        const std::string key = rowArtworkKey(*desired[i]);
        if (m_rowArtwork.find(key) == m_rowArtwork.end())
            submitDecode(*desired[i], i == 0, true);
    }
}

void HomeScreen::tryLoadOneRowArtwork()
{
    if (activeTabNamed("Shows")) {
        updateShowsDecodeWorkingSet();
        return;
    }
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;

    // Movies is a flattened 9x4 grid: m_rowScroll is a grid row, not a
    // TabData row.  Decode only its actual visible cached posters.
    if (activeTabNamed("Movies")) {
        const MediaRow &row = rows[0];
        int first = m_rowScroll * MOVIE_GRID_COLUMNS;
        int last = std::min((int)row.items.size(), first + MOVIE_GRID_COLUMNS * MOVIE_GRID_ROWS);
        for (int i=first; i<last; ++i) {
            const MediaItem &item=row.items[i]; std::string key=rowArtworkKey(item);
            if (key.empty() || m_rowArtwork.find(key)!=m_rowArtwork.end()) continue;
            submitDecode(item,i==first,false);
        }
        return;
    }

    // Submit every horizontally visible card that is not already in the RAM
    // cache. submitDecode preserves outstanding-job de-duplication and the
    // bounded queue, while selected artwork was already queued at priority.
    static constexpr int HMARGIN = 4;
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int cardAccumX = HMARGIN;
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            ArtworkBox sz = artworkBoxSize(row.items[ci]);
            int screenX = cardAccumX - m_cardScroll;
            if (screenX + sz.w < HMARGIN) {
                cardAccumX += sz.w + CARD_GAP;
                continue;
            }
            if (screenX > 640 - HMARGIN) break;
            std::string key = rowArtworkKey(row.items[ci]);
            if (!key.empty() && m_rowArtwork.find(key) == m_rowArtwork.end())
                submitDecode(row.items[ci], false, false);
            cardAccumX += sz.w + CARD_GAP;
        }
    }
}

} // namespace miyoofin
