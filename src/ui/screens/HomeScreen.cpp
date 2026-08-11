#include "HomeScreen.hpp"
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

// Selected artwork box origin (top-left of info panel)
static constexpr int ART_X = 8;
static constexpr int ART_Y = INFO_Y + 6;   // 32
static void blitDecoded(SDL_Surface *fb, const DecodedImage &img, int x, int y, int w, int h) {
    if (img.empty()) return;
    float ia=(float)img.width/img.height, ba=(float)w/h;
    int dw=ia>ba?w:(int)(h*ia+.5f), dh=ia>ba?(int)(w/ia+.5f):h;
    SDL_Surface *s=SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(),img.width,img.height,32,img.width*4,0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    if(s){SDL_Rect src={0,0,img.width,img.height},dst={x+(w-dw)/2,y+(h-dh)/2,dw,dh};SDL_BlitScaled(s,&src,fb,&dst);SDL_FreeSurface(s);}
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

std::vector<TabData> HomeScreen::tabsFromSnapshot(const LibrarySnapshot &s)
{
    std::vector<TabData> tabs;
    std::vector<MediaRow> home;
    if (!s.continueWatching.empty()) home.push_back({"Continue Watching", s.continueWatching});
    if (!s.recentlyAdded.empty()) home.push_back({"Recently Added", s.recentlyAdded});
    if (home.empty()) home.push_back({"", {}});
    tabs.push_back({"Home", std::move(home)});
    TabData movies{"Movies", {{"Movies", combineMovieViews(s.movies)}}};
    if (movies.rows.empty()) movies.rows.push_back({"No movies found", {}});
    tabs.push_back(std::move(movies));
    TabData shows{"Shows", {}}; for (const auto &v : s.shows) shows.rows.push_back({v.name, v.items});
    if (shows.rows.empty()) shows.rows.push_back({"No shows found", {}});
    tabs.push_back(std::move(shows));
    tabs.push_back({"Downloads", {{"", {}}}});
    return tabs;
}

std::vector<std::string> HomeScreen::tabNames(const std::vector<TabData> &tabs)
{ std::vector<std::string> names; for (const auto &tab : tabs) names.push_back(tab.name); return names; }

std::vector<TabData> HomeScreen::offlineTabsFromSnapshot(const LibrarySnapshot &snapshot)
{
    std::vector<TabData> tabs=tabsFromSnapshot(snapshot);
    tabs.erase(std::remove_if(tabs.begin(),tabs.end(),[](const TabData &tab) {
        return tab.name=="Home" || tab.name=="Search";
    }),tabs.end());
    return tabs;
}

int HomeScreen::transitionTabIndex(const std::vector<TabData> &from, int selected,
                                   const std::vector<TabData> &to)
{
    const std::string name=(selected>=0 && selected<(int)from.size()) ? from[selected].name : "";
    for (int i=0; i<(int)to.size(); ++i) if (to[i].name==name) return i;
    for (int i=0; i<(int)to.size(); ++i) if (to[i].name=="Movies") return i;
    return to.empty() ? 0 : std::min(std::max(selected,0),(int)to.size()-1);
}

std::vector<MediaItem> HomeScreen::combineMovieViews(const std::vector<CachedLibraryView> &views)
{ std::vector<MediaItem> out; std::map<std::string,bool> seen; for(const auto&v:views)for(const auto&i:v.items)if(seen.emplace(i.id,true).second)out.push_back(i); std::sort(out.begin(),out.end(),movieOrganizationalLess); return out; }

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

void HomeScreen::rebuildShowsPresentation() { ShowsPresentation p=makeShowsPresentation((m_libraryOffline?m_offlineSnapshot:m_cachedSnapshot).shows); m_showMaster=std::move(p.shows); m_animeMaster=std::move(p.anime); refreshShowsFilter(); }
void HomeScreen::prepareOfflineProjection() { OfflineCatalogSnapshot catalog; const bool catalogLoaded=OfflineCatalog::load(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),catalog,nullptr); if(catalogLoaded){std::lock_guard<std::mutex> lock(m_catalogSnapshotMutex);m_catalogSnapshot=catalog;m_catalogSnapshotReady=true;} OfflineLibraryProjection p(m_cachedSnapshot,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_fetchOfflineTabs=offlineTabsFromSnapshot(m_cachedSnapshot);m_fetchOfflineMovies=p.movies();m_fetchOfflineSnapshot=m_cachedSnapshot;for(auto &view:m_fetchOfflineSnapshot.shows){std::vector<MediaItem>filtered;for(const auto&i:view.items)if(p.playable(i.id)||!p.seasons(i.id).empty())filtered.push_back(i);view.items=std::move(filtered);}m_fetchOfflinePrepared=true; }
void HomeScreen::applyOfflineProjection() { const std::vector<TabData> previous=m_tabs;const int selected=m_activeTab;if(!m_fetchOfflinePrepared)return;m_tabs=std::move(m_fetchOfflineTabs);m_activeTab=transitionTabIndex(previous,selected,m_tabs);m_movieMaster=std::move(m_fetchOfflineMovies);m_offlineSnapshot=std::move(m_fetchOfflineSnapshot);m_fetchOfflinePrepared=false;refreshMovieFilter();rebuildShowsPresentation();clampNavigation(); }
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
        if (m_libraryOffline) {
            catalog.seasonsBySeries.emplace(seriesId,seasons);
            for (const auto &season:seasons) {
                const auto episodesIt=m_catalogSnapshot.episodesBySeason.find(season.id);
                if (episodesIt!=m_catalogSnapshot.episodesBySeason.end())
                    catalog.episodesBySeason.emplace(season.id,episodesIt->second);
            }
        }
    }
    if (m_libraryOffline) {
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
        if (LibraryCache::load(path, m_cachedSnapshot)) {
            m_tabs = tabsFromSnapshot(m_cachedSnapshot); m_haveCachedSnapshot = true;
            m_movieMaster = combineMovieViews(m_cachedSnapshot.movies);
            refreshMovieFilter();
            rebuildShowsPresentation();
            m_loadState = LoadState::Ready; clampNavigation();
            printf("[HomeScreen] Loaded local library cache\n");
        }
        const std::string scope=LibraryCache::scopeKey(m_session.serverUrl,m_session.userId);
        const bool haveState=SyncStateStore::load(SyncStateStore::path("cache",scope),m_syncState);
        m_forceHierarchyReconcile=!haveState || !syncStateFresh(m_syncState,wallClockMs(),HIERARCHY_RECONCILE_MS);
        if (haveState && syncStateFresh(m_syncState,wallClockMs(),SYNC_FRESH_WALL_MS)) {
            // Persisted freshness survives process lifetime; cache is already
            // rendered above, so a relaunch does not begin another crawl.
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
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if(activeTabNamed("Downloads")&&m_downloads)m_downloads->requestReconcile(); return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (activeTabNamed("Movies") || activeTabNamed("Shows")) requestFetch(SDL_GetTicks()); if(activeTabNamed("Downloads")&&m_downloads)m_downloads->requestReconcile(); return true;
    case Action::Search: return false;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        if(activeTabNamed("Shows")){if(m_showsFocus==ShowsFocus::AlphabetRail){m_showsActiveLetter=m_showsActiveLetter==m_showsAlphabetFocus?-1:m_showsAlphabetFocus;refreshShowsFilter();return true;}if(const MediaItem*i=showsSelectedItem()){m_stack->push(std::make_unique<SeriesScreen>(m_session,*i,m_downloads,m_libraryOffline,cachedSeasonsForSeries(i->id)));return true;}return true;}
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
                m_stack->push(std::make_unique<SeriesScreen>(m_session, *item,m_downloads,m_libraryOffline,cachedSeasonsForSeries(item->id)));
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
                        m_session, series, season, item->id,m_downloads,m_libraryOffline));
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

void HomeScreen::refreshDownloads()
{
    if (m_downloadRefreshDone) finishDownloadRefresh();
    if (!m_downloadRefreshInFlight && m_downloadRefreshTimer == 0)
        startDownloadRefresh();
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

void HomeScreen::finishDownloadRefresh()
{
    UiDiagnostics::Scope scope("HomeScreen::publishDownloadSnapshot");
    if(m_downloadRefreshThread.joinable())m_downloadRefreshThread.join();
    m_downloadRefreshDone=false;m_downloadRefreshInFlight=false;
    m_downloadSnapshot=std::move(m_downloadRefreshResult);
    m_missingJournalEntries=std::move(m_downloadJournalResult);
    m_downloadRefreshTimer=500;
    m_downloadHierarchy=buildDownloadHierarchy(m_downloadSnapshot,m_downloadExpanded);
    const auto &rows=m_downloadHierarchy.visible;
    m_downloadSelected=downloadHierarchySelection(rows,m_downloadSelectedId,m_downloadSelected);
    m_downloadScroll=clampDownloadScroll(m_downloadSelected,(int)rows.size(),m_downloadScroll,5);
    m_downloadSelectedId=rows.empty()?"":rows[m_downloadSelected].id;
    if (!m_downloadConfirmId.empty() && m_downloadConfirmId != m_downloadSelectedId) {
        m_downloadConfirmId.clear();
        m_downloadConfirmItemIds.clear();
    }
    if (!m_journalDiscardConfirmId.empty() && (m_missingJournalEntries.empty() || m_missingJournalEntries.front().itemId != m_journalDiscardConfirmId))
        m_journalDiscardConfirmId.clear();
}

bool HomeScreen::handleDownloadsAction(Action action)
{
    const auto &rows=m_downloadHierarchy.visible;
    if (action == Action::Up || action == Action::Down) {
        m_downloadSelected += action == Action::Up ? -1 : 1;
        m_downloadSelected = clampDownloadSelection(m_downloadSelected, (int)rows.size());
        m_downloadScroll = clampDownloadScroll(m_downloadSelected, (int)rows.size(), m_downloadScroll, 5);
        m_downloadSelectedId = rows.empty() ? "" : rows[m_downloadSelected].id;
        m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
        return true;
    }
    if (rows.empty()) {
        if (action == Action::Search && !m_missingJournalEntries.empty()) {
            const std::string &id=m_missingJournalEntries.front().itemId;
            if (m_journalDiscardConfirmId == id) {
                const std::string path=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
                OfflinePlaybackJournal::discardMissing(path,id,nullptr);
                m_journalDiscardConfirmId.clear(); refreshDownloads();
            } else m_journalDiscardConfirmId=id;
            return true;
        }
        return action == Action::Confirm || action == Action::ActionsMenu;
    }
    const DownloadHierarchyRow &row=rows[m_downloadSelected];
    if (action == Action::Back && !m_downloadConfirmId.empty()) {
        m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear(); return true;
    }
    if ((row.kind==DownloadHierarchyRowKind::Series || row.kind==DownloadHierarchyRowKind::Season) && action==Action::Confirm) {
        if(row.expanded) m_downloadExpanded.erase(row.id); else m_downloadExpanded.insert(row.id);
        m_downloadHierarchy=buildDownloadHierarchy(m_downloadSnapshot,m_downloadExpanded);
        m_downloadSelected=downloadHierarchySelection(m_downloadHierarchy.visible,row.id,m_downloadSelected);
        m_downloadScroll=clampDownloadScroll(m_downloadSelected,(int)m_downloadHierarchy.visible.size(),m_downloadScroll,5);
        m_downloadSelectedId=row.id; m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear(); return true;
    }
    // Y belongs to Downloads even when the selected row has no removal
    // operation (such as Series/Season parents or malformed leaves).  Letting
    // it fall through would trigger Home's global logout action.
    if (action == Action::ActionsMenu && downloadHierarchyConsumesActionsMenu(row)) {
        if (row.item && downloadCanRemove(*row.item)) {
            const DownloadItem &item=*row.item;
            if (m_downloadConfirmId == row.id) {
                m_downloads->erase(item.itemId, nullptr);
                m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
            } else { m_downloadConfirmId = row.id; m_downloadConfirmItemIds={item.itemId}; }
        } else if (downloadHierarchyIsBulkRemovalParent(row)) {
            if (downloadHierarchyBulkRemovalConfirmed(m_downloadConfirmId,row)) {
                // DownloadManager::erase cancels an active transfer before it
                // removes its local manifest/segments and store entry.
                for (const std::string &itemId : m_downloadConfirmItemIds)
                    m_downloads->erase(itemId, nullptr);
                m_downloadConfirmId.clear(); m_downloadConfirmItemIds.clear();
            } else {
                m_downloadConfirmItemIds=downloadHierarchyBulkRemovalItemIds(row,m_downloadSnapshot);
                if (!m_downloadConfirmItemIds.empty()) m_downloadConfirmId=row.id;
            }
        }
        return true;
    }
    if (!row.item) return action==Action::Confirm;
    const DownloadItem &item=*row.item;
    if (action == Action::Search && item.state == DownloadState::UpdateAvailable) { m_downloads->redownload(item.itemId); return true; }
    if (action == Action::Search && !m_missingJournalEntries.empty()) {
        const std::string &id=m_missingJournalEntries.front().itemId;
        if (m_journalDiscardConfirmId == id) {
            const std::string path=OfflinePlaybackJournal::path("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
            OfflinePlaybackJournal::discardMissing(path,id,nullptr);
            m_journalDiscardConfirmId.clear();
            refreshDownloads();
        } else m_journalDiscardConfirmId=id;
        return true;
    }
    if (action != Action::Confirm) return false;
    switch (downloadPrimaryControl(item.state)) {
    case DownloadPrimaryControl::Pause: m_downloads->pause(item.itemId); return true;
    case DownloadPrimaryControl::Resume: m_downloads->resume(item.itemId); return true;
    case DownloadPrimaryControl::Retry: m_downloads->retry(item.itemId); return true;
    case DownloadPrimaryControl::Play: {
        std::string error;
        const char *type = item.itemType == "episode" ? "episode" : "movie";
        if (PlaybackRequest::writeWithSourceTo(PlaybackRequest::defaultPath(), item.itemId, type,
                item.playbackPositionTicks, "local", m_downloads->scope(), error))
            m_stack->requestExternalPlayback();
        return true;
    }
    default: return true;
    }
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
    if (m_loadState == LoadState::Ready && activeTabNamed("Downloads")) refreshDownloads();
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

void HomeScreen::render(SDL_Surface *fb)
{
    drawTabBar(fb);

    if (m_loadState == LoadState::Loading) {
        drawLoadingState(fb);
        drawBottomHints(fb);
        return;
    }
    if (m_loadState == LoadState::Error) {
        drawErrorState(fb);
        drawBottomHints(fb);
        return;
    }

    // Ready
    const TabData &tab = currentTab();
    if (activeTabNamed("Downloads")) {
        drawDownloadsTab(fb);
    } else if (tab.rows.size() == 1 && tab.rows[0].items.empty()) {
        drawPlaceholderTab(fb, tab.rows[0].label.empty() ? "No content" : tab.rows[0].label.c_str());
    } else {
        bool hasItems = false;
        for (const auto &r : tab.rows)
            if (!r.items.empty()) { hasItems = true; break; }
        if (activeTabNamed("Movies")) {
            drawMoviePreview(fb); drawMovieAlphabetRail(fb); drawMovieGrid(fb);
        } else if (activeTabNamed("Shows")) {
            drawShowsPreview(fb); drawShowsAlphabetRail(fb); drawShowsGrid(fb);
        } else if (!hasItems) {
            drawPlaceholderTab(fb, tab.name == "Movies" ?
                "No movies on this server" :
                tab.name == "Shows" ? "No shows on this server" : "No content");
        } else {
            drawInfoPanel(fb); drawRowList(fb);
        }
    }
    drawBottomHints(fb);
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

    std::string url   = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid   = m_session.userId;
    std::string devId = m_session.deviceId;

    m_fetchThread = std::thread([this, url, token, uid, devId]() {
        std::string err;
        auto fail=[&](const std::string &error) {
            m_fetchError=error;
            if(m_haveCachedSnapshot)prepareOfflineProjection();
            m_fetchDone=true;
        };

        // 1. Get library views
        std::vector<LibraryView> views;
        if (!JellyfinApi::getViews(url, token, uid, devId, views, err)) {
            fail(err);
            return;
        }
        printf("[HomeScreen] Got %zu library views\n", views.size());

        // 2. Continue watching (non-fatal if fails)
        std::vector<MediaItem> cw;
        std::string cwErr;
        if (!JellyfinApi::getResumeItems(url, token, uid, devId, 12, cw, cwErr))
            printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());

        // 3. Recently added (non-fatal)
        std::vector<MediaItem> ra;
        std::string raErr;
        if (!JellyfinApi::getLatestItems(url, token, uid, devId, 16, ra, raErr))
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
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Movie", 50, items, ie)) {
                    moviesByView.push_back({v.name, std::move(items)});
                    snapshot.movies.push_back({v.id, v.name, v.collectionType, moviesByView.back().second});
                } else { fail(ie); return; }
            } else if (v.collectionType == "tvshows") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Series", 50, items, ie)) {
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
            if (!JellyfinApi::getChangedHierarchyItems(url,token,uid,devId,m_syncState.lastSuccessfulMs,changed,changedError)) {
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
    std::vector<PosterJob> out; std::set<std::string> seen;
    auto add=[&](const MediaItem &item) { DisplayArtwork a=displayArtworkForItem(item); if(!a.valid()) return; PosterJob j{item.id,a.imageType,a.tag,a.width,a.height}; std::string key=buildRowArtworkKey(item); if(seen.insert(key).second && !ImageCache::isCached(j.itemId,j.imageType,j.imageTag,j.width,j.height)) out.push_back(std::move(j)); };
    for(const auto &views : {&snapshot.movies,&snapshot.shows}) for(const auto &view:*views) for(const auto &item:view.items) add(item);
    for(const auto &item:snapshot.continueWatching) add(item);
    for(const auto &item:snapshot.recentlyAdded) add(item);
    return out;
}

std::vector<HomeScreen::PosterJob> HomeScreen::collectSeasonPosterJobs(const std::vector<MediaItem> &seasons)
{
    std::vector<PosterJob> out; std::set<std::string> seen;
    for (const auto &season : seasons) {
        if (!season.type.empty() && season.type != "season") continue;
        auto tag=season.imageTags.find("Primary");
        if (season.id.empty() || tag==season.imageTags.end() || tag->second.empty()) continue;
        PosterJob job{season.id,ImageType::Primary,tag->second,SEASON_POSTER_W,SEASON_POSTER_H};
        std::string key=job.itemId+":"+job.imageTag;
        if (seen.insert(key).second && !ImageCache::isCached(job.itemId,job.imageType,job.imageTag,job.width,job.height)) out.push_back(std::move(job));
    }
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
            if (!JellyfinApi::getSeasons(m_session.serverUrl,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,seasons,error)) { if(generation==m_hierarchyGeneration.load()) m_hierarchyOffline.store(true); continue; }
            queuePosterJobs(collectSeasonPosterJobs(seasons));
            std::map<std::string,std::vector<MediaItem> > episodesBySeason;
            bool complete=true;
            for (const auto &season : seasons) {
                { std::lock_guard<std::mutex> lock(m_hierarchyMutex); if(m_stopHierarchyWorker)return; }
                if (season.id.empty()) { complete=false; break; }
                std::vector<MediaItem> episodes; error.clear();
                if (!JellyfinApi::getEpisodes(m_session.serverUrl,m_session.accessToken,m_session.userId,m_session.deviceId,series.id,season.id,episodes,error)) { complete=false; if(generation==m_hierarchyGeneration.load()) m_hierarchyOffline.store(true); break; }
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
        CURLM *multi=curl_multi_init(); if(!multi) continue; size_t next=0; std::map<CURL*,PosterTransfer*> active;
        auto launch=[&](const PosterJob &job){ auto *t=new PosterTransfer; t->job=job; CURL *easy=curl_easy_init(); if(!easy){delete t;return;} for(const auto&h:JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId))t->headers=curl_slist_append(t->headers,h.c_str()); t->url=buildImageUrl(m_session.serverUrl,job.itemId,job.imageType,job.imageTag,job.width,job.height); curl_easy_setopt(easy,CURLOPT_URL,t->url.c_str()); curl_easy_setopt(easy,CURLOPT_WRITEFUNCTION,posterWrite);curl_easy_setopt(easy,CURLOPT_WRITEDATA,t);curl_easy_setopt(easy,CURLOPT_HTTPHEADER,t->headers);curl_easy_setopt(easy,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(easy,CURLOPT_TIMEOUT,8L);curl_easy_setopt(easy,CURLOPT_CONNECTTIMEOUT,8L);curl_easy_setopt(easy,CURLOPT_FOLLOWLOCATION,1L);curl_easy_setopt(easy,CURLOPT_SSL_VERIFYPEER,0L);curl_easy_setopt(easy,CURLOPT_SSL_VERIFYHOST,0L);curl_multi_add_handle(multi,easy);active[easy]=t; };
        while(next<jobs.size()||!active.empty()) { while(next<jobs.size()&&active.size()<POSTER_MAX_CONCURRENT)launch(jobs[next++]); int running=0;curl_multi_perform(multi,&running);int n=0;while(CURLMsg*msg=curl_multi_info_read(multi,&n)){if(msg->msg!=CURLMSG_DONE)continue;CURL*easy=msg->easy_handle;auto*t=active[easy];long status=0;curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);if(msg->data.result==CURLE_OK&&status>=200&&status<300&&!t->tooLarge&&!t->bytes.empty())ImageCache::writeToCache(t->job.itemId,t->job.imageType,t->job.imageTag,t->job.width,t->job.height,t->bytes.data(),t->bytes.size());curl_multi_remove_handle(multi,easy);curl_easy_cleanup(easy);curl_slist_free_all(t->headers);delete t;active.erase(easy);}if(!active.empty()){int fds=0;curl_multi_wait(multi,nullptr,0,100,&fds);} }
        curl_multi_cleanup(multi);
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

    std::string url = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    LibrarySnapshot snapshot=m_cachedSnapshot;
    const std::string cachePath=LibraryCache::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId));
    m_resumeRefreshThread = std::thread([this, url, token, uid, devId, snapshot, cachePath]() mutable {
        std::vector<MediaItem> items;
        std::string error;
        if (JellyfinApi::getResumeItems(url, token, uid, devId, 12,
                                        items, error)) {
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
    return buildRowArtworkKey(item);
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

void HomeScreen::drawTabBar(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, TAB_Y, 640, TAB_H,
        Theme::BG_R*2/3, Theme::BG_G*2/3, Theme::BG_B*2/3, 255);
    BitmapFont::fillRect(fb, 0, TAB_Y+TAB_H-1, 640, 1,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 100);
    int x = 8, tabY = TAB_Y + (TAB_H - BitmapFont::GLYPH_H)/2;
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        const char *name = m_tabs[i].name.c_str();
        if (i == m_activeTab) {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            int tw = (int)::strlen(name) * BitmapFont::GLYPH_W;
            BitmapFont::fillRect(fb, x, TAB_Y+TAB_H-3, tw, 2,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,200);
        } else {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
        }
        x += (int)::strlen(name) * BitmapFont::GLYPH_W + 16;
    }
    std::string status = syncStatusText();
    std::string login = "Logged in as: " + m_userName;
    const int maxChars = 24;
    if ((int)login.size() > maxChars) login = login.substr(0, maxChars - 3) + "...";
    int loginX = 640 - 8 - (int)login.size() * BitmapFont::GLYPH_W;
    // Do not obscure tabs on unusually narrow font/theme combinations.
    if (loginX > x + 4)
        BitmapFont::drawString(fb, loginX, tabY, login.c_str(), Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R*2/3, Theme::BG_G*2/3, Theme::BG_B*2/3);
    int statusX=loginX-8-(int)status.size()*BitmapFont::GLYPH_W;
    if (!status.empty() && statusX > x+4)
        BitmapFont::drawString(fb,statusX,tabY,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,
                               Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
}

std::string HomeScreen::syncStatusText() const
{
    return librarySyncStatus(m_activeTab,m_haveCachedSnapshot,
        m_libraryOffline || m_hierarchyOffline.load(),m_syncSchedule.inFlight,
        m_syncSchedule.hasSucceeded,{m_hierarchyCompleted.load(),m_hierarchyTotal.load()},
        m_hierarchyActive.load());
}

void HomeScreen::drawInfoPanel(SDL_Surface *fb)
{
    const MediaItem *item = currentItem();
    if (!item) return;
    BitmapFont::fillRect(fb,0,INFO_Y,640,INFO_H,24,24,32,255);
    BitmapFont::fillRect(fb,0,INFO_Y,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
    ArtworkBox box = artworkBoxSize(*item);
    int px=ART_X, py=ART_Y, pw=box.w, ph=box.h;
    // Placeholder colour behind everything
    BitmapFont::fillRect(fb,px,py,pw,ph,item->artR,item->artG,item->artB,255);

    // Render decoded artwork if available, aspect-fit centred
    if (!m_selectedArtwork.empty()) {
        int imgW = m_selectedArtwork.width;
        int imgH = m_selectedArtwork.height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)pw / (float)ph;
        int drawW, drawH;
        if (imgAspect > boxAspect) {
            // Wider than box — fit to width
            drawW = pw;
            drawH = (int)(pw / imgAspect + 0.5f);
            if (drawH > ph) drawH = ph;
        } else {
            // Taller than box — fit to height
            drawH = ph;
            drawW = (int)(ph * imgAspect + 0.5f);
            if (drawW > pw) drawW = pw;
        }
        int drawX = px + (pw - drawW) / 2;
        int drawY = py + (ph - drawH) / 2;

        // Create an SDL surface wrapping the RGBA pixel data.
        // The DecodedImage (m_selectedArtwork) keeps the pixels alive.
        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_selectedArtwork.pixels.data(),
            imgW, imgH,
            32,                    // bits per pixel
            imgW * 4,              // pitch (bytes per row)
            0x000000FF,            // R mask
            0x0000FF00,            // G mask
            0x00FF0000,            // B mask
            0xFF000000);           // A mask
        if (imgSurface) {
            SDL_Rect srcRect = {0, 0, imgW, imgH};
            SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
            SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
            SDL_FreeSurface(imgSurface);
        }
    }

    BitmapFont::drawRect(fb,px,py,pw,ph,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    int mx=px+pw+10, my=py+2;
    BitmapFont::drawString(fb,mx,my,item->title.c_str(),
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char line1[128];
    std::snprintf(line1,sizeof(line1),"%d  |  %s",item->year,item->genre.c_str());
    BitmapFont::drawString(fb,mx,my,line1,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char rs[32];
    int fs=(int)item->rating, idx=0;
    for (int s=0;s<fs;++s) rs[idx++]='*';
    if (item->rating-fs>=0.25f) rs[idx++]='.';
    rs[idx]='\0';
    BitmapFont::drawString(fb,mx,my,rs,
        Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
    char rn[16];
    std::snprintf(rn,sizeof(rn),"  %.1f/5.0",item->rating);
    BitmapFont::drawString(fb,mx+(int)::strlen(rs)*BitmapFont::GLYPH_W,my,rn,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char ov[256];
    std::snprintf(ov,sizeof(ov),"%s",item->overview.c_str());
    if ((int)::strlen(ov) > 55) { ov[55]='\0'; std::strcat(ov,"..."); }
    BitmapFont::drawString(fb,mx,my,ov,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    BitmapFont::fillRect(fb,0,INFO_Y+INFO_H,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
}

void HomeScreen::drawRowList(SDL_Surface *fb)
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;
    static constexpr int VGAP = 4;
    static constexpr int HMARGIN = 4;
    for (int ri=0; ri<VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int rowY = ROWS_Y + ri * (ROW_LABEL_H + ROW_STRIP_H + VGAP + 4);
        int caY = rowY + ROW_LABEL_H;
        char label[64];
        std::snprintf(label,sizeof(label),"  %s",row.label.c_str());
        BitmapFont::drawString(fb,4,rowY,label,
            rowIdx==m_activeRow?Theme::ACCENT_R:Theme::TEXT_R,
            rowIdx==m_activeRow?Theme::ACCENT_G:Theme::TEXT_G,
            rowIdx==m_activeRow?Theme::ACCENT_B:Theme::TEXT_B,
            Theme::BG_R,Theme::BG_G,Theme::BG_B);
        // Draw cards with per-item sizing and pixel-scroll offset
        int cardAccumX = HMARGIN;
        for (int ci=0; ci<(int)row.items.size(); ++ci) {
            ArtworkBox sz = artworkBoxSize(row.items[ci]);
            int screenX = cardAccumX - m_cardScroll;
            if (screenX + sz.w < HMARGIN) {
                // Fully off-screen left
                cardAccumX += sz.w + CARD_GAP;
                continue;
            }
            if (screenX > 640 - HMARGIN) break;
            bool sel = (rowIdx==m_activeRow && ci==m_activeCard);
            int cardScreenY = caY + (ROW_STRIP_H - sz.h) / 2;
            drawCard(fb,screenX,cardScreenY,sz.w,sz.h,row.items[ci],sel);
            cardAccumX += sz.w + CARD_GAP;
        }
    }
}

void HomeScreen::drawMovieGrid(SDL_Surface *fb)
{
    const MediaRow *row = currentRow(); if (!row) return;
    const int top = 134, gap = 6;
    for (int i=0; i<(int)row->items.size(); ++i) {
        int gr = i / MOVIE_GRID_COLUMNS, gc = i % MOVIE_GRID_COLUMNS;
        if (gr < m_rowScroll || gr >= m_rowScroll + MOVIE_GRID_ROWS) continue;
        int x = 42 + gc * (64 + gap), y = top + (gr - m_rowScroll) * (96 + gap);
        drawCard(fb, x, y, 64, 96, row->items[i], !m_movieRailFocused && i == m_activeCard);
    }
    if (row->items.empty() && m_movieActiveLetter >= 0) {
        char message[48]; std::snprintf(message, sizeof(message), "No movies starting with %c", 'A' + m_movieActiveLetter);
        BitmapFont::drawString(fb, 48, 210, message, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }
}

void HomeScreen::drawMovieAlphabetRail(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, 25, 36, 437, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 35, 25, 1, 437, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    for (int i = 0; i < 26; ++i) {
        int y = 27 + i * 16;
        bool focused = m_movieRailFocused && i == m_movieAlphabetFocus;
        bool active = i == m_movieActiveLetter;
        if (focused) BitmapFont::fillRect(fb, 2, y - 1, 31, BitmapFont::GLYPH_H + 2, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 120);
        char letter[2] = {static_cast<char>('A' + i), '\0'};
        BitmapFont::drawString(fb, 14, y, letter,
            active ? Theme::HIGHLIGHT_R : focused ? Theme::BG_R : Theme::TEXT_R,
            active ? Theme::HIGHLIGHT_G : focused ? Theme::BG_G : Theme::TEXT_G,
            active ? Theme::HIGHLIGHT_B : focused ? Theme::BG_B : Theme::TEXT_B,
            focused ? Theme::ACCENT_R : 24, focused ? Theme::ACCENT_G : 24, focused ? Theme::ACCENT_B : 32);
        if (active && !focused) BitmapFont::fillRect(fb, 4, y + BitmapFont::GLYPH_H + 1, 27, 1, Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B, 255);
    }
}

void HomeScreen::drawMoviePreview(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 36, 25, 604, 105, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 36, 129, 604, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 70);
    const MediaItem *item = currentItem();
    if (!item) return;
    int px = 42, py = 29;
    BitmapFont::fillRect(fb, px, py, 64, 96, item->artR, item->artG, item->artB, 255);
    if (!m_selectedArtwork.empty()) {
        SDL_Surface *image = SDL_CreateRGBSurfaceFrom((void *)m_selectedArtwork.pixels.data(), m_selectedArtwork.width, m_selectedArtwork.height, 32, m_selectedArtwork.width * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
        if (image) { SDL_Rect src={0,0,m_selectedArtwork.width,m_selectedArtwork.height}, dst={px,py,64,96}; SDL_BlitScaled(image,&src,fb,&dst); SDL_FreeSurface(image); }
    }
    BitmapFont::drawRect(fb, px, py, 64, 96, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
    int x=114, y=33;
    char title[68]; std::snprintf(title, sizeof(title), "%s", item->title.c_str());
    if ((int)std::strlen(title) > 64) { title[61]='.'; title[62]='.'; title[63]='.'; title[64]='\0'; }
    BitmapFont::drawString(fb, x, y, title, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 24,24,32);
    char meta[96] = {}; int n=0;
    if (item->year > 0) n += std::snprintf(meta+n, sizeof(meta)-n, "%d", item->year);
    int mins=ticksToMinutes(item->runTimeTicks); if (mins > 0) n += std::snprintf(meta+n, sizeof(meta)-n, "%s%dh %dm", n ? " * " : "", mins/60, mins%60);
    if (item->rating > 0) std::snprintf(meta+n, sizeof(meta)-n, "%s%.1f", n ? " * " : "", (double)item->rating);
    BitmapFont::drawString(fb, x, y+18, meta, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,24,24,32);
    char state[96]; std::snprintf(state, sizeof(state), "%s%s", item->genre.c_str(), item->played ? (item->genre.empty()?"Watched":" * Watched") : item->progress > 0 ? "" : "");
    if (item->progress > 0 && !item->played) std::snprintf(state, sizeof(state), "%s%s%d%% watched", item->genre.c_str(), item->genre.empty()?"":" * ", (int)(item->progress*100.0f+0.5f));
    BitmapFont::drawString(fb, x, y+36, state, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,24,24,32);
}

void HomeScreen::drawShowsAlphabetRail(SDL_Surface *fb) {
    BitmapFont::fillRect(fb,0,25,SHOWS_RAIL_W,437,24,24,32,255); BitmapFont::fillRect(fb,35,25,1,437,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,90);
    for(int i=0;i<26;++i){int y=27+i*16;bool f=m_showsFocus==ShowsFocus::AlphabetRail&&i==m_showsAlphabetFocus,a=i==m_showsActiveLetter;if(f)BitmapFont::fillRect(fb,2,y-1,31,BitmapFont::GLYPH_H+2,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,120);char c[2]={char('A'+i),0};BitmapFont::drawString(fb,14,y,c,a?Theme::HIGHLIGHT_R:f?Theme::BG_R:Theme::TEXT_R,a?Theme::HIGHLIGHT_G:f?Theme::BG_G:Theme::TEXT_G,a?Theme::HIGHLIGHT_B:f?Theme::BG_B:Theme::TEXT_B,f?Theme::ACCENT_R:24,f?Theme::ACCENT_G:24,f?Theme::ACCENT_B:32);}
}
void HomeScreen::drawShowsPreview(SDL_Surface *fb) {
    BitmapFont::fillRect(fb,36,25,604,SHOWS_PREVIEW_H,24,24,32,255); BitmapFont::fillRect(fb,36,129,604,1,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,70); const MediaItem*item=showsSelectedItem();if(!item)return;int px=42,py=29;BitmapFont::fillRect(fb,px,py,64,96,item->artR,item->artG,item->artB,255); std::string key=rowArtworkKey(*item);auto it=m_rowArtwork.find(key);if(it!=m_rowArtwork.end()&&it->second.status==RowArtworkStatus::Loaded&&it->second.image)blitDecoded(fb,*it->second.image,px,py,64,96);else blitDecoded(fb,m_selectedArtwork,px,py,64,96);BitmapFont::drawRect(fb,px,py,64,96,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);BitmapFont::drawString(fb,114,33,item->title.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);char meta[96]={};int n=0;if(item->year)n+=std::snprintf(meta+n,sizeof(meta)-n,"%d",item->year);if(item->rating>0)std::snprintf(meta+n,sizeof(meta)-n,"%s%.1f",n?" * ":"",(double)item->rating);BitmapFont::drawString(fb,114,51,meta,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);char state[96];std::snprintf(state,sizeof(state),"%s%s",item->genre.c_str(),item->played?" * Watched":item->progress>0?" * In progress":"");BitmapFont::drawString(fb,114,69,state,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
}
void HomeScreen::drawShowsGrid(SDL_Surface *fb) {
    BitmapFont::drawString(fb,44,137,"SHOWS",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);BitmapFont::drawString(fb,346,137,"ANIME",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);BitmapFont::fillRect(fb,337,135,1,327,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,100);auto draw=[&](const std::vector<MediaItem>&v,int scroll,int sel,bool focused,int base){for(int i=0;i<(int)v.size();++i){int r=i/4;if(r<scroll||r>=scroll+3)continue;drawCard(fb,base+14+(i%4)*70,SHOWS_GRID_TOP+(r-scroll)*102,64,96,v[i],focused&&i==sel);}};draw(m_filteredShows,m_showScroll,m_showSelected,m_showsFocus==ShowsFocus::ShowsGrid,SHOWS_LEFT_X);draw(m_filteredAnime,m_animeScroll,m_animeSelected,m_showsFocus==ShowsFocus::AnimeGrid,SHOWS_RIGHT_X);if(m_filteredShows.empty()&&m_filteredAnime.empty()){char b[64];if(m_showsActiveLetter>=0)std::snprintf(b,sizeof(b),"No shows or anime starting with %c",'A'+m_showsActiveLetter);else std::snprintf(b,sizeof(b),"No shows on this server");BitmapFont::drawString(fb,48,230,b,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);}
}

void HomeScreen::drawCard(SDL_Surface *fb,int x,int y,int w,int h,
                          const MediaItem &item,bool selected)
{
    BitmapFont::fillRect(fb,x,y,w,h,item.artR,item.artG,item.artB,255);

    // B5d2b: render loaded row artwork over the placeholder
    {
        std::string key = rowArtworkKey(item);
        if (!key.empty()) {
            auto it = m_rowArtwork.find(key);
            if (it != m_rowArtwork.end()
                && it->second.status == RowArtworkStatus::Loaded
                && it->second.image
                && !it->second.image->empty())
            {
                const DecodedImage &img = *it->second.image;
                int imgW = img.width;
                int imgH = img.height;
                float imgAspect = (float)imgW / (float)imgH;
                float boxAspect = (float)w / (float)h;
                int drawW, drawH;
                if (imgAspect > boxAspect) {
                    drawW = w;
                    drawH = (int)(w / imgAspect + 0.5f);
                    if (drawH > h) drawH = h;
                } else {
                    drawH = h;
                    drawW = (int)(h * imgAspect + 0.5f);
                    if (drawW > w) drawW = w;
                }
                int drawX = x + (w - drawW) / 2;
                int drawY = y + (h - drawH) / 2;

                SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
                    (void *)img.pixels.data(),
                    imgW, imgH,
                    32,
                    imgW * 4,
                    0x000000FF,
                    0x0000FF00,
                    0x00FF0000,
                    0xFF000000);
                if (imgSurface) {
                    SDL_Rect srcRect = {0, 0, imgW, imgH};
                    SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
                    SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
                    SDL_FreeSurface(imgSurface);
                }
            }
        }
    }

    int ty = y + h - BitmapFont::GLYPH_H - 2;
    BitmapFont::fillRect(fb,x,ty,w,BitmapFont::GLYPH_H+2,0,0,0,160);
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s",item.title.c_str());
    int mcc = (w-4)/BitmapFont::GLYPH_W;
    if ((int)::strlen(buf) > mcc) {
        buf[mcc-1]='.'; buf[mcc-2]='.'; buf[mcc]='\0';
    }
    BitmapFont::drawString(fb,x+2,ty+1,buf,255,255,255,0,0,0);
    if (selected) {
        BitmapFont::drawRect(fb,x-2,y-2,w+4,h+4,255,220,40);
        BitmapFont::drawRect(fb,x-1,y-1,w+2,h+2,255,255,120);
    } else {
        BitmapFont::drawRect(fb,x,y,w,h,
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    }
}

void HomeScreen::drawPlaceholderTab(SDL_Surface *fb, const char *message)
{
    BitmapFont::drawString(fb,8,200,message,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
        Theme::BG_R,Theme::BG_G,Theme::BG_B);
}

void HomeScreen::drawDownloadsTab(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, 25, 640, 437, 24, 24, 32, 255);
    char summary[128];
    std::snprintf(summary, sizeof(summary), "Free %s | Local %s | Queue %s",
        formatBytes(m_downloadSnapshot.freeBytes).c_str(),
        formatBytes(m_downloadSnapshot.localBytes).c_str(),
        formatBytes(m_downloadSnapshot.reservedBytes).c_str());
    BitmapFont::drawString(fb, 8, 32, summary, Theme::ACCENT_R, Theme::ACCENT_G,
        Theme::ACCENT_B, 24, 24, 32);
    BitmapFont::fillRect(fb, 8, 49, 624, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    const auto &rows=m_downloadHierarchy.visible;
    if (rows.empty()) {
        BitmapFont::drawString(fb, 8, 210, "No downloads", Theme::TEXT_R, Theme::TEXT_G,
            Theme::TEXT_B, 24, 24, 32);
        if (!m_missingJournalEntries.empty()) BitmapFont::drawString(fb,8,230,"Missing offline progress: X=Discard",Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
        return;
    }
    bool moviesLabel=false, showsLabel=false;
    for (int visible=0; visible<5; ++visible) {
        int index=m_downloadScroll+visible; if(index >= (int)rows.size()) break;
        const DownloadHierarchyRow &row=rows[index]; const DownloadItem *item=row.item;
        int y=58+visible*76; bool selected=index==m_downloadSelected;
        const bool movie=row.kind==DownloadHierarchyRowKind::Movie;
        if(movie&&!moviesLabel) { BitmapFont::drawString(fb,8,y,"Movies",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32); y+=11; moviesLabel=true; }
        if(!movie&&!showsLabel) { BitmapFont::drawString(fb,8,y,"Shows",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32); y+=11; showsLabel=true; }
        if(selected) BitmapFont::fillRect(fb, 6+row.indent*14, y-2, 628-row.indent*14, 58, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 80);
        const int x=12+row.indent*14;
        std::string title=row.title;
        if(row.kind==DownloadHierarchyRowKind::Series || row.kind==DownloadHierarchyRowKind::Season) title+=(row.expanded?"  v":"  >");
        if(title.size()>68-(size_t)row.indent*2) title.resize(68-(size_t)row.indent*2);
        BitmapFont::drawString(fb, x, y, title.c_str(), selected?Theme::ACCENT_R:Theme::TEXT_R,
            selected?Theme::ACCENT_G:Theme::TEXT_G, selected?Theme::ACCENT_B:Theme::TEXT_B,24,24,32);
        std::string detail;
        if(!item) {
            detail=std::to_string(row.aggregate.episodes)+" episodes";
            if(row.aggregate.active) detail+=" | "+std::to_string(row.aggregate.progress)+"%";
            else if(row.aggregate.complete) detail+=" | "+std::to_string(row.aggregate.complete)+" complete";
            if(row.aggregate.bytesKnown) detail+=" | "+formatBytes(row.aggregate.bytes)+"/"+formatBytes(row.aggregate.totalBytes);
        } else {
        const bool completedHls=item->hlsStorage && downloadHierarchyComplete(*item);
        const std::string sizeLabel=(item->hlsStorage&&!completedHls?"~":"")+formatBytes(displayDownloadBytes(*item));
        if(item->itemType=="episode") detail=episodeDownloadLabel(*item);
        else detail=std::string(downloadStateLabel(item->state))+" | "+sizeLabel;
        if(item->state==DownloadState::Downloading) {
            char active[96]; std::snprintf(active,sizeof(active),"Downloading %u%% | %s/s",downloadPercent(*item),formatBytes(item->recentBytesPerSec).c_str()); detail=active;
        } else if(item->itemType=="episode") detail += " | "+std::string(downloadStateLabel(item->state))+" | "+sizeLabel;
        if(!item->lastError.empty() && (item->state==DownloadState::Failed || item->state==DownloadState::WaitingForNetwork || item->state==DownloadState::Unauthorized))
            detail=std::string(downloadStateLabel(item->state))+" | "+item->lastError;
        }
        if(detail.size()>74) detail.resize(74);
        BitmapFont::drawString(fb, x, y+18, detail.c_str(), Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
        BitmapFont::fillRect(fb, x, y+39, 600-row.indent*14, 3, 48,48,58,255);
        int fill=(int)((600-row.indent*14)*(item?downloadPercent(*item):(row.aggregate.progressKnown?row.aggregate.progress:0))/100);
        if(fill) BitmapFont::fillRect(fb,x,y+39,fill,3,Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,255);
    }
}

void HomeScreen::drawBottomHints(SDL_Surface *fb)
{
    int y = 480 - BOTTOM_H;
    BitmapFont::fillRect(fb,0,y,640,BOTTOM_H,
        Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3,255);

    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb,8,y+2,"Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb,8,y+2,"A=Retry  Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_logoutArmed && !m_logoutRequested) {
        int secs = (m_logoutTimer + 999) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)", secs);
        BitmapFont::drawString(fb,8,y+2,buf,
            Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else {
        const char *hints = "A=Select  B=Back  L/R=Tabs  Y=Logout";
        if (activeTabNamed("Downloads")) {
            if (!m_journalDiscardConfirmId.empty()) {
                BitmapFont::drawString(fb,8,y+2,"Press X again to discard missing progress",Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
                return;
            }
            const DownloadHierarchyRow *selectedRow=m_downloadSelected>=0&&m_downloadSelected<(int)m_downloadHierarchy.visible.size()?&m_downloadHierarchy.visible[m_downloadSelected]:nullptr;
            const DownloadItem *selectedItem=selectedRow?selectedRow->item:nullptr;
            if (!m_downloadConfirmId.empty() && selectedRow && m_downloadConfirmId==selectedRow->id) {
                char confirm[96];
                if (selectedItem) {
                    const DownloadItem &item=*selectedItem;
                    std::snprintf(confirm,sizeof(confirm),"Press Y again to %s %s",downloadRemoveIsDelete(item)?"delete":"cancel",formatBytes(displayDownloadBytes(item)).c_str());
                } else {
                    std::snprintf(confirm,sizeof(confirm),"Press Y again to delete %s (%u local)",selectedRow->kind==DownloadHierarchyRowKind::Season?"Season":"entire Series",(unsigned)m_downloadConfirmItemIds.size());
                }
                BitmapFont::drawString(fb,8,y+2,confirm,Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
                return;
            }
            const char *primary="";
            if (selectedItem) primary=downloadPrimaryControlLabel(downloadPrimaryControl(selectedItem->state));
            bool update=selectedItem && selectedItem->state==DownloadState::UpdateAvailable;
            if(selectedRow && !selectedItem) primary=selectedRow->expanded?"Collapse":"Expand";
            const bool parent=selectedRow && !selectedItem;
            char downloadHints[96]; std::snprintf(downloadHints,sizeof(downloadHints),update?"A=Play  X=Update  Y=Delete  B=Back":(parent?"A=%s  Y=Delete all  B=Back":(!m_missingJournalEntries.empty()?"A=%s  X=Discard missing progress  Y=Cancel/Delete":"A=%s  Y=Cancel/Delete  B=Back")),primary[0]?primary:"Select"); hints=downloadHints;
            BitmapFont::drawString(fb,8,y+2,hints,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            return;
        }
        if (activeTabNamed("Movies")) {
            hints = m_movieRailFocused
                ? "A=Filter  Right=Movies  B=Back"
                : "A=Select  Left@edge=Alphabet  L/R=Tabs";
        } else if (activeTabNamed("Shows")) {
            hints = m_showsFocus == ShowsFocus::AlphabetRail
                ? "A=Filter  Right=Shows  B=Back"
                : "A=Select  Left/Right=Move  Edge=Alphabet";
        }
        BitmapFont::drawString(fb,8,y+2, hints,
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    }
}

void HomeScreen::drawLoadingState(SDL_Surface *fb)
{
    char userBuf[64];
    std::snprintf(userBuf, sizeof(userBuf), "Logged in as %s",
                  m_userName.c_str());
    int ux = (fb->w - (int)::strlen(userBuf) * BitmapFont::GLYPH_W) / 2;
    int uy = fb->h / 3;
    BitmapFont::drawString(fb, ux, uy, userBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    static int dotPhase = 0;
    dotPhase = (dotPhase + 1) % 60;
    int dots = dotPhase / 15;
    char buf[32] = "Loading library";
    for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
    int mx = (fb->w - (int)::strlen(buf) * BitmapFont::GLYPH_W) / 2;
    int my = uy + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, mx, my, buf,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void HomeScreen::drawErrorState(SDL_Surface *fb)
{
    const char *header = "Failed to load library";
    int hx = (fb->w - (int)::strlen(header) * BitmapFont::GLYPH_W) / 2;
    int hy = fb->h / 3;
    BitmapFont::drawString(fb, hx, hy, header,
        Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    char errBuf[128];
    std::snprintf(errBuf, sizeof(errBuf), "%s", m_fetchError.c_str());
    int maxChars = (640 - 16) / BitmapFont::GLYPH_W;
    if ((int)::strlen(errBuf) > maxChars) errBuf[maxChars] = '\0';
    int ex = (fb->w - (int)::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
    int ey = hy + BitmapFont::GLYPH_H + 8;
    BitmapFont::drawString(fb, ex, ey, errBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    const char *hint = "Press A to retry";
    int hix = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
    int hiy = ey + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, hix, hiy, hint,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
