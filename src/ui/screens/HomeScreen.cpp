#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
#include "../MovieTitle.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/ScreenStack.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <atomic>
#include <chrono>
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
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;

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

struct PosterTransfer { MediaItem item; std::string url; std::vector<unsigned char> bytes; curl_slist *headers=nullptr; bool tooLarge=false; };
static size_t posterWrite(void *p, size_t s, size_t n, void *u) {
    PosterTransfer *t=static_cast<PosterTransfer*>(u); size_t z=s*n;
    if (z > POSTER_MAX_BYTES-t->bytes.size()) { t->tooLarge=true; return 0; }
    const unsigned char *b=static_cast<const unsigned char*>(p); t->bytes.insert(t->bytes.end(),b,b+z); return z;
}

// Selected artwork box origin (top-left of info panel)
static constexpr int ART_X = 8;
static constexpr int ART_Y = INFO_Y + 6;   // 32
// Width and height are now computed per-item via artworkBoxSize()

HomeScreen::HomeScreen(const Session &session)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_userName(session.userName)
{
    // Placeholder tabs until fetch completes
    m_tabs.push_back({"Home", {{"", {}}}});
    m_tabs.push_back({"Movies", {{"", {}}}});
    m_tabs.push_back({"Shows", {{"", {}}}});
    m_tabs.push_back({"Search", {{"", {}}}});
    m_tabs.push_back({"Downloads", {{"", {}}}});
}

HomeScreen::~HomeScreen()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();
    if (m_posterThread.joinable()) m_posterThread.join();
}

std::vector<TabData> HomeScreen::tabsFromSnapshot(const LibrarySnapshot &s)
{
    std::vector<TabData> tabs;
    tabs.push_back({"Home", {{"", {}}}});
    TabData movies{"Movies", {{"Movies", combineMovieViews(s.movies)}}};
    if (movies.rows.empty()) movies.rows.push_back({"No movies found", {}});
    tabs.push_back(std::move(movies));
    TabData shows{"Shows", {}}; for (const auto &v : s.shows) shows.rows.push_back({v.name, v.items});
    if (shows.rows.empty()) shows.rows.push_back({"No shows found", {}});
    tabs.push_back(std::move(shows));
    tabs.push_back({"Search", {{"", {}}}}); tabs.push_back({"Downloads", {{"", {}}}});
    return tabs;
}

std::vector<MediaItem> HomeScreen::combineMovieViews(const std::vector<CachedLibraryView> &views)
{ std::vector<MediaItem> out; std::map<std::string,bool> seen; for(const auto&v:views)for(const auto&i:v.items)if(seen.emplace(i.id,true).second)out.push_back(i); std::sort(out.begin(),out.end(),movieOrganizationalLess); return out; }

void HomeScreen::refreshMovieFilter()
{
    if (m_tabs.size() <= 1) return;
    std::vector<MediaItem> displayed;
    for (const auto &item : m_movieMaster) {
        if (movieMatchesAlphabetFilter(item.title, m_movieActiveLetter))
            displayed.push_back(item);
    }
    m_tabs[1].rows = {{"Movies", std::move(displayed)}};
    m_activeRow = 0; m_activeCard = 0; m_rowScroll = 0; m_cardScroll = 0;
    m_selectedArtwork = {}; m_selectedArtworkId.clear(); m_selectedArtworkAttempted = false;
}

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
    return m_tabs[m_activeTab];
}

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
    if (m_activeTab == 1) {
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
            m_loadState = LoadState::Ready; clampNavigation();
            printf("[HomeScreen] Loaded local library cache\n");
        }
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
    switch (action) {
    case Action::Up:
        if (m_activeTab == 1) { if (m_movieRailFocused) { if (m_movieAlphabetFocus > 0) --m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),-1,0); }
        else m_activeRow--;
        clampNavigation(); return true;
    case Action::Down:
        if (m_activeTab == 1) { if (m_movieRailFocused) { if (m_movieAlphabetFocus < 25) ++m_movieAlphabetFocus; } else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),1,0); }
        else m_activeRow++;
        clampNavigation(); return true;
    case Action::Left:
        if (m_activeTab == 1) {
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
        if (m_activeTab == 1) { if (m_movieRailFocused) m_movieRailFocused=false; else if (currentRow()) m_activeCard=moveMovieGridCompact(m_activeCard,(int)currentRow()->items.size(),0,1); }
        else m_activeCard++;
        clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (m_activeTab==1 || m_activeTab==2) requestFetch(SDL_GetTicks()); return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_movieRailFocused = false;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); if (m_activeTab==1 || m_activeTab==2) requestFetch(SDL_GetTicks()); return true;
    case Action::Search:
        m_activeTab = 3; m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0; m_movieRailFocused = false; return true;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        if (m_activeTab == 1 && m_movieRailFocused) {
            m_movieActiveLetter = m_movieActiveLetter == m_movieAlphabetFocus ? -1 : m_movieAlphabetFocus;
            refreshMovieFilter();
            return true;
        }
        const MediaItem *item = currentItem();
        if (item) {
            printf("[HomeScreen] Select: %s (%s)\n",
                   item->title.c_str(), item->type.c_str());
            if (item->type == "show") {
                m_stack->push(std::make_unique<SeriesScreen>(m_session, *item));
                return true;
            }
            if (item->type == "movie") {
                m_stack->push(std::make_unique<MovieDetailsScreen>(m_session, *item));
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
                        m_session, series, season, item->id));
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
        if (m_activeTab == 1 && m_movieRailFocused) {
            m_movieRailFocused = false;
            return true;
        }
        return false;
    default: return false;
    }
}

void HomeScreen::update(Uint32 dt)
{
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) { m_logoutTimer = 0; m_logoutArmed = false; }
        else m_logoutTimer -= dt;
    }
    if (m_fetchDone)
        finishFetch();
    if (m_loadState == LoadState::Ready && m_resumeRefreshDone)
        finishResumeRefresh();

    // Attempt selected artwork load (identity guard prevents repeats)
    if (m_loadState == LoadState::Ready)
        tryLoadSelectedArtwork();

    // B5d2a: load at most one new row artwork per update cycle
    if (m_loadState == LoadState::Ready)
        tryLoadOneRowArtwork();
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
    if (m_activeTab == 3 || m_activeTab == 4) {
        drawPlaceholderTab(fb, m_activeTab == 3 ?
            "Search - not yet implemented" :
            "Downloads - not yet implemented");
    } else if (tab.rows.size() == 1 && tab.rows[0].items.empty()
               && tab.rows[0].label.empty()) {
        drawPlaceholderTab(fb, "No content");
    } else {
        bool hasItems = false;
        for (const auto &r : tab.rows)
            if (!r.items.empty()) { hasItems = true; break; }
        if (m_activeTab == 1) {
            drawMoviePreview(fb); drawMovieAlphabetRail(fb); drawMovieGrid(fb);
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

    std::string url   = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid   = m_session.userId;
    std::string devId = m_session.deviceId;

    m_fetchThread = std::thread([this, url, token, uid, devId]() {
        std::string err;

        // 1. Get library views
        std::vector<LibraryView> views;
        if (!JellyfinApi::getViews(url, token, uid, devId, views, err)) {
            m_fetchError = err;
            m_fetchDone = true;
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
        for (const auto &v : views) {
            if (v.collectionType == "movies") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Movie", 50, items, ie)) {
                    moviesByView.push_back({v.name, std::move(items)});
                    snapshot.movies.push_back({v.id, v.name, v.collectionType, moviesByView.back().second});
                } else { m_fetchError=ie; m_fetchDone=true; return; }
            } else if (v.collectionType == "tvshows") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Series", 50, items, ie)) {
                    showsByView.push_back({v.name, std::move(items)});
                    snapshot.shows.push_back({v.id, v.name, v.collectionType, showsByView.back().second});
                } else { m_fetchError=ie; m_fetchDone=true; return; }
            }
        }

        m_fetchResult = JellyfinApi::buildTabs(
            views, cw, ra, moviesByView, showsByView);
        m_remoteSnapshot = std::move(snapshot);
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
        if (!m_haveCachedSnapshot) m_loadState = LoadState::Error;
        printf("[HomeScreen] Fetch failed: %s\n", m_fetchError.c_str());
        if (m_syncSchedule.complete(SDL_GetTicks())) startFetch();
        return;
    }
    std::vector<StalePoster> stale;
    ReconcileStats stats = LibraryCache::reconcile(m_cachedSnapshot, m_remoteSnapshot, &stale);
    std::string path = LibraryCache::cachePath("cache", LibraryCache::scopeKey(m_session.serverUrl, m_session.userId));
    if (!LibraryCache::save(path, m_remoteSnapshot)) {
        printf("[HomeScreen] Library cache save failed; retaining old cache\n");
    } else { m_cachedSnapshot = m_remoteSnapshot; m_haveCachedSnapshot = true;
        for (const auto &p : stale) ImageCache::removeCached(p.itemId, ImageType::Primary, p.tag, 64, 96);
        startPosterSync(m_cachedSnapshot);
    }
    m_tabs = std::move(m_fetchResult);
    m_movieMaster = combineMovieViews(m_cachedSnapshot.movies);
    refreshMovieFilter();
    m_loadState = LoadState::Ready;
    clampNavigation();
    printf("[HomeScreen] Library loaded: %zu tabs (%d added, %d changed)\n", m_tabs.size(), stats.added, stats.changed);
    if (m_syncSchedule.complete(SDL_GetTicks())) startFetch();
}

void HomeScreen::startPosterSync(const LibrarySnapshot &snapshot)
{
    if (m_posterThread.joinable()) m_posterThread.join();
    std::vector<MediaItem> jobs; for(const auto& group : {snapshot.movies, snapshot.shows}) for(const auto&v:group) for(const auto&i:v.items) { auto p=i.imageTags.find("Primary"); if(p!=i.imageTags.end()&&!p->second.empty()&&!ImageCache::isCached(i.id,ImageType::Primary,p->second,64,96)) jobs.push_back(i); }
    std::string url=m_session.serverUrl, token=m_session.accessToken, dev=m_session.deviceId;
    m_posterThread=std::thread([jobs=std::move(jobs),url,token,dev](){
        CURLM *multi=curl_multi_init(); if(!multi) return; size_t next=0; int running=0;
        std::map<CURL*,PosterTransfer*> active;
        auto launch=[&](const MediaItem&i){ PosterTransfer *t=new PosterTransfer; t->item=i; auto tag=i.imageTags.find("Primary");
            CURL *easy=curl_easy_init(); if(!easy){delete t;return;} for(const auto&h:JellyfinApi::buildAuthHeaders(token,dev))t->headers=curl_slist_append(t->headers,h.c_str());
            t->url=buildImageUrl(url,i.id,ImageType::Primary,tag->second,64,96); curl_easy_setopt(easy,CURLOPT_URL,t->url.c_str());
            curl_easy_setopt(easy,CURLOPT_WRITEFUNCTION,posterWrite); curl_easy_setopt(easy,CURLOPT_WRITEDATA,t); curl_easy_setopt(easy,CURLOPT_HTTPHEADER,t->headers);
            curl_easy_setopt(easy,CURLOPT_NOSIGNAL,1L); curl_easy_setopt(easy,CURLOPT_TIMEOUT,8L); curl_easy_setopt(easy,CURLOPT_CONNECTTIMEOUT,8L); curl_easy_setopt(easy,CURLOPT_FOLLOWLOCATION,1L); curl_easy_setopt(easy,CURLOPT_MAXREDIRS,5L); curl_easy_setopt(easy,CURLOPT_SSL_VERIFYPEER,0L); curl_easy_setopt(easy,CURLOPT_SSL_VERIFYHOST,0L);
            curl_multi_add_handle(multi,easy); active[easy]=t; };
        while(next<jobs.size() || !active.empty()) { while(next<jobs.size() && active.size()<POSTER_MAX_CONCURRENT) launch(jobs[next++]); curl_multi_perform(multi,&running); int num=0; CURLMsg *msg; while((msg=curl_multi_info_read(multi,&num))){if(msg->msg!=CURLMSG_DONE)continue; CURL *easy=msg->easy_handle; PosterTransfer*t=active[easy];long status=0;curl_easy_getinfo(easy,CURLINFO_RESPONSE_CODE,&status);auto tag=t->item.imageTags.find("Primary");if(msg->data.result==CURLE_OK&&status>=200&&status<300&&!t->tooLarge&&!t->bytes.empty())ImageCache::writeToCache(t->item.id,ImageType::Primary,tag->second,64,96,t->bytes.data(),t->bytes.size());curl_multi_remove_handle(multi,easy);curl_easy_cleanup(easy);curl_slist_free_all(t->headers);delete t;active.erase(easy);} if(!active.empty()){int numfds=0;curl_multi_wait(multi,nullptr,0,200,&numfds);if(numfds==0)std::this_thread::sleep_for(std::chrono::milliseconds(1));} }
        curl_multi_cleanup(multi);
    });
}

void HomeScreen::startResumeRefresh()
{
    if (m_resumeRefreshThread.joinable())
        m_resumeRefreshThread.join();

    m_resumeRefreshDone = false;
    m_resumeRefreshInFlight = true;
    m_resumeRefreshSucceeded = false;
    m_resumeRefreshError.clear();
    m_resumeRefreshResult.clear();

    std::string url = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    m_resumeRefreshThread = std::thread([this, url, token, uid, devId]() {
        std::vector<MediaItem> items;
        std::string error;
        if (JellyfinApi::getResumeItems(url, token, uid, devId, 12,
                                        items, error)) {
            m_resumeRefreshResult = std::move(items);
            m_resumeRefreshSucceeded = true;
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
    const MediaItem *item = currentItem();
    if (!item) {
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    // Per-type artwork box dimensions
    ArtworkBox box = artworkBoxSize(*item);

    // Look for a Primary image tag
    auto it = item->imageTags.find("Primary");
    if (it == item->imageTags.end() || it->second.empty()) {
        // No Primary tag — clear artwork, keep placeholder
        m_selectedArtwork = {};
        m_selectedArtworkId.clear();
        m_selectedArtworkAttempted = false;
        return;
    }

    // Build identity key from item id + primary tag
    std::string key = item->id + ":" + it->second;

    // Already attempted this exact selection?  Do not retry.
    if (m_selectedArtworkAttempted && m_selectedArtworkId == key)
        return;

    // New selection — reset and attempt once
    m_selectedArtwork = {};
    m_selectedArtworkId = key;
    // Local library artwork remains retryable until the poster worker writes
    // it; Home retains its historical one-shot network behavior below.
    m_selectedArtworkAttempted = false;

    const std::string &tag = it->second;
    printf("[HomeScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           item->id.c_str(), tag.c_str(), box.w, box.h);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    if (ImageCache::isCached(item->id, ImageType::Primary, tag, box.w, box.h)) {
        jpegData = ImageCache::readCached(item->id, ImageType::Primary, tag, box.w, box.h);
        printf("[HomeScreen] Artwork: cache hit (%zu bytes)\n", jpegData.size());
    }

    // Movies/Shows are deliberately local-first: their artwork is only
    // populated by the background library sync, never by navigation.
    if ((m_activeTab == 1 || m_activeTab == 2) && jpegData.empty()) return;
    m_selectedArtworkAttempted = true;
    // 2. Dynamic Home artwork may still use its existing request path.
    if (jpegData.empty()) {
        std::string url = buildImageUrl(
            m_session.serverUrl, item->id, ImageType::Primary, tag, box.w, box.h);

        HttpClient client;
        client.setTimeoutSec(8);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        if (!client.getBinary(url, headers, response, error, 512 * 1024)) {
            printf("[HomeScreen] Artwork fetch failed: %s\n", error.c_str());
            return;
        }
        if (!response.ok()) {
            printf("[HomeScreen] Artwork HTTP %ld\n", response.status);
            return;
        }

        jpegData = std::move(response.data);
        printf("[HomeScreen] Artwork: downloaded %zu bytes\n", jpegData.size());

        // Cache to disk (best-effort)
        ImageCache::writeToCache(item->id, ImageType::Primary, tag, box.w, box.h,
                                 jpegData.data(), jpegData.size());
    }

    // 3. Decode JPEG
    m_selectedArtwork = ImageDecoder::decodeJpeg(jpegData.data(), jpegData.size());
    if (m_selectedArtwork.empty()) {
        printf("[HomeScreen] Artwork: decode failed\n");
    } else {
        printf("[HomeScreen] Artwork: decoded %dx%d\n",
               m_selectedArtwork.width, m_selectedArtwork.height);
    }
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
    while ((int)m_rowArtworkOrder.size() > ROW_ARTWORK_RAM_LIMIT) {
        std::string oldKey = m_rowArtworkOrder.front();
        m_rowArtworkOrder.erase(m_rowArtworkOrder.begin());

        m_rowArtwork.erase(oldKey);
    }
}

void HomeScreen::tryLoadOneRowArtwork()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;

    // Movies is a flattened 9x4 grid: m_rowScroll is a grid row, not a
    // TabData row.  Decode only its actual visible cached posters.
    if (m_activeTab == 1) {
        const MediaRow &row = rows[0];
        int first = m_rowScroll * MOVIE_GRID_COLUMNS;
        int last = std::min((int)row.items.size(), first + MOVIE_GRID_COLUMNS * MOVIE_GRID_ROWS);
        int attempts = 0;
        for (int i=first; i<last && attempts<MOVIE_ARTWORK_DECODE_BUDGET; ++i) {
            const MediaItem &item=row.items[i]; std::string key=rowArtworkKey(item);
            if (key.empty() || m_rowArtwork.find(key)!=m_rowArtwork.end()) continue;
            auto tag=item.imageTags.find("Primary");
            if (tag==item.imageTags.end() || !ImageCache::isCached(item.id,ImageType::Primary,tag->second,64,96)) continue;
            ++attempts;
            auto data=ImageCache::readCached(item.id,ImageType::Primary,tag->second,64,96);
            DecodedImage image=ImageDecoder::decodeJpeg(data.data(),data.size());
            if (image.empty()) { m_rowArtwork[key].status=RowArtworkStatus::Failed; continue; }
            m_rowArtwork[key].status=RowArtworkStatus::Loaded;
            m_rowArtwork[key].image=std::move(image); m_rowArtworkOrder.push_back(key); evictRowArtworkIfNeeded();
        }
        return;
    }

    // Scan horizontally visible cards and find ONE not-yet-attempted candidate
    static constexpr int HMARGIN = 4;
    std::string candidate;
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
            if (!key.empty() && m_rowArtwork.find(key) == m_rowArtwork.end()) {
                if (candidate.empty())
                    candidate = key;
            }
            cardAccumX += sz.w + CARD_GAP;
        }
    }

    if (candidate.empty()) return;

    // Find the matching item to get dimensions and tag
    const MediaItem *matchItem = nullptr;
    for (int ri = 0; ri < VISIBLE_ROWS && !matchItem; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            if (rowArtworkKey(row.items[ci]) == candidate) {
                matchItem = &row.items[ci];
                break;
            }
        }
    }
    if (!matchItem) return;

    ArtworkBox box = artworkBoxSize(*matchItem);
    auto tagIt = matchItem->imageTags.find("Primary");
    if (tagIt == matchItem->imageTags.end()) return;
    const std::string &tag = tagIt->second;
    const std::string &itemId = matchItem->id;

    printf("[HomeScreen] RowArtwork: loading %s (%dx%d)\n",
           candidate.c_str(), box.w, box.h);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    if (ImageCache::isCached(itemId, ImageType::Primary, tag, box.w, box.h))
        jpegData = ImageCache::readCached(itemId, ImageType::Primary, tag, box.w, box.h);

    if ((m_activeTab == 1 || m_activeTab == 2) && jpegData.empty()) return;
    // A real read/decode failure is the only permanent failure state.
    m_rowArtwork[candidate].status = RowArtworkStatus::Failed;
    // 2. Dynamic Home artwork may still use its existing request path.
    if (jpegData.empty()) {
        std::string url = buildImageUrl(
            m_session.serverUrl, itemId, ImageType::Primary, tag, box.w, box.h);

        HttpClient client;
        client.setTimeoutSec(5);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        if (!client.getBinary(url, headers, response, error, 256 * 1024)) {
            printf("[HomeScreen] RowArtwork fetch failed: %s\n", error.c_str());
            return;
        }
        if (!response.ok()) {
            printf("[HomeScreen] RowArtwork HTTP %ld\n", response.status);
            return;
        }
        jpegData = std::move(response.data);
        ImageCache::writeToCache(itemId, ImageType::Primary, tag, box.w, box.h,
                                 jpegData.data(), jpegData.size());
    }

    // 3. Decode JPEG
    DecodedImage img = ImageDecoder::decodeJpeg(jpegData.data(), jpegData.size());
    if (img.empty()) {
        printf("[HomeScreen] RowArtwork: decode failed\n");
        return;  // status already Failed
    }

    printf("[HomeScreen] RowArtwork: decoded %s (%dx%d)\n",
           candidate.c_str(), img.width, img.height);

    // Store decoded image
    m_rowArtwork[candidate].status = RowArtworkStatus::Loaded;
    m_rowArtwork[candidate].image = std::move(img);
    m_rowArtworkOrder.push_back(candidate);
    evictRowArtworkIfNeeded();
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
                && !it->second.image.empty())
            {
                const DecodedImage &img = it->second.image;
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
        const char *hints = "A=Select  B=Back  L/R=Tabs  X=Search  Y=Logout";
        if (m_activeTab == 1) {
            hints = m_movieRailFocused
                ? "A=Filter  Right=Movies  B=Back"
                : "A=Select  Left@edge=Alphabet  L/R=Tabs";
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
