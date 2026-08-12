#include "SeriesScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../cache/OfflineCatalog.hpp"
#include "../../cache/LibraryCache.hpp"
#include "../../cache/OfflineLibraryProjection.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

// -------------------------------------------------------------------
// Layout constants — 640×480 framebuffer
// -------------------------------------------------------------------
static constexpr int FB_W           = 640;
static constexpr int FB_H           = 480;
static constexpr int BOTTOM_H       = 18;

// Top-left heading
static constexpr int HEAD_X         = 22;
static constexpr int HEAD_Y         = 16;

// Season poster grid
static constexpr int COL_X[2]       = { 48, 154 };
static constexpr int GRID_TOP_Y     = 51;
static constexpr int GRID_ROW_H     = 135;   // y=51, y=186, y=321
static constexpr int GRID_ROWS      = 3;     // visible rows
static constexpr int GRID_COLS      = 2;
static constexpr int GRID_VISIBLE   = GRID_ROWS * GRID_COLS; // 6
static constexpr int POSTER_W       = 74;
static constexpr int POSTER_H       = 111;
static constexpr int OVERLAY_H      = 18;    // title bar at bottom of poster

// Right-side show poster placeholder
static constexpr int SHOW_X         = 360;
static constexpr int SHOW_Y         = 51;
static constexpr int SHOW_W         = 160;
static constexpr int SHOW_H         = 240;

// Metadata under the show poster
static constexpr int META_X         = 363;
static constexpr int META_Y         = 305;
static constexpr int META_WRAP      = 24;    // chars before wrapping overview

/// Word-wrap text into lines of at most wrapCols characters.
/// Breaks at spaces when possible; hard-breaks long words.
static std::vector<std::string> wrapOverview(const char *text, int wrapCols)
{
    std::vector<std::string> lines;
    if (!text || !*text) return lines;

    std::string input(text);
    size_t pos = 0;

    while (pos < input.size()) {
        size_t newline = input.find('\n', pos);
        std::string para = (newline != std::string::npos)
            ? input.substr(pos, newline - pos)
            : input.substr(pos);

        while (!para.empty()) {
            if ((int)para.size() <= wrapCols) {
                lines.push_back(para);
                break;
            }
            size_t lastSpace = para.rfind(' ', wrapCols);
            if (lastSpace != std::string::npos && lastSpace > 0) {
                lines.push_back(para.substr(0, lastSpace));
                para = para.substr(lastSpace + 1);
            } else {
                lines.push_back(para.substr(0, wrapCols));
                para = para.substr(wrapCols);
            }
        }

        if (newline != std::string::npos)
            pos = newline + 1;
        else
            break;
    }

    return lines;
}

std::string SeriesScreen::seasonArtworkKey(const MediaItem &season)
{
    auto it = season.imageTags.find("Primary");
    if (it == season.imageTags.end() || it->second.empty())
        return {};
    return season.id + ":" + it->second + ":" + std::to_string(POSTER_W) + "x" + std::to_string(POSTER_H);
}

SeriesScreen::SeriesScreen(const Session &session, const MediaItem &series, std::shared_ptr<DownloadManager> downloads, bool offline,
                           std::vector<MediaItem> cachedSeasons)
    : m_session(session)
    , m_series(series)
    , m_downloads(std::move(downloads))
    , m_offline(offline)
    , m_seasons(std::move(cachedSeasons))
{
    if (!m_seasons.empty()) m_loadState=LoadState::Ready;
}

void SeriesScreen::enter()
{
    UiDiagnostics::Scope scope("SeriesScreen::enter");
    printf("[SeriesScreen] enter series=%s\n", m_series.title.c_str());
    if (m_seasons.empty()) {
        // ScreenStack::push calls enter() synchronously on the SDL thread.
        // Catalog reads can parse a large file (and the offline projection also
        // snapshots DownloadManager), so leave this screen immediately visible
        // and let the existing fetch worker provide cached seasons afterwards.
        m_loadState=LoadState::Loading;
        fetchSeasons(true);
    } else {
        // The Home hierarchy worker already supplied a filtered in-memory
        // cache.  Keep it interactive while the normal network refresh runs.
        fetchSeasons();
    }
    tryLoadSeriesArtwork();
}
SeriesScreen::~SeriesScreen()
{
    leave();
    if(m_fetchThread.joinable())m_fetchThread.join();
    if(m_artworkThread.joinable())m_artworkThread.join();
    freePreparedArtwork(m_seriesArtworkSurface);
    for (auto &entry : m_seasonArtworkSurfaces)
        freePreparedArtwork(entry.second);
}

std::vector<MediaItem> SeriesScreen::replaceSeasonsKeepingSelection(const std::vector<MediaItem> &current,
                                                                      std::vector<MediaItem> fresh, int &selected)
{
    std::string selectedId=current.empty()?"":current[std::min(std::max(selected,0),(int)current.size()-1)].id;
    int n=0;
    for(;n<(int)fresh.size()&&fresh[n].id!=selectedId;n++);
    selected=n<(int)fresh.size()?n:0;
    return fresh;
}

void SeriesScreen::leave()
{
    if(m_shutdownSignalled.exchange(true,std::memory_order_acq_rel))return;
    UiDiagnostics::Scope scope("SeriesScreen::workerShutdown");
    printf("[SeriesScreen] leave series=%s\n", m_series.title.c_str());
    m_fetchCancelled.store(true, std::memory_order_release);
    m_artworkCancelled.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> g(m_artworkMutex); m_artworkStop=true; }
    m_artworkCv.notify_one();
}

void SeriesScreen::fetchSeasons(bool loadCachedSeasons)
{
    if(m_fetchThread.joinable()) { std::lock_guard<std::mutex>g(m_fetchMutex); if(!m_fetchDone)return; m_fetchThread.join(); }
    if(m_seasons.empty()) m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchDone=false;m_cachedSeasonsDone=false;}
    m_fetchCancelled.store(false, std::memory_order_release); Session s=m_session; std::string id=m_series.id;
    const bool offline=m_offline, loadCached=loadCachedSeasons;
    const std::string catalogPath=OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(s.serverUrl,s.userId));
    std::shared_ptr<DownloadManager> downloads=m_downloads;
    const MediaItem series=m_series;
    m_fetchThread=std::thread([this,s,id,offline,loadCached,catalogPath,downloads,series](){
        if(loadCached) {
            OfflineCatalogSnapshot catalog;
            OfflineCatalog::load(catalogPath,catalog,nullptr);
            if(m_fetchCancelled.load(std::memory_order_acquire)) return;
            std::vector<MediaItem> cached;
            if(offline) {
                LibrarySnapshot library;
                OfflineLibraryProjection projection(library,catalog,downloads?downloads->snapshot():DownloadSnapshot{});
                cached=projection.seasons(id);
            } else {
                auto it=catalog.seasonsBySeries.find(id);
                if(it!=catalog.seasonsBySeries.end()) cached=it->second;
            }
            {std::lock_guard<std::mutex>g(m_fetchMutex);m_cachedSeasons=std::move(cached);m_cachedSeasonsDone=true;}
            if(offline) { std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=true;m_fetchDone=true;return; }
        }
        if(m_fetchCancelled.load(std::memory_order_acquire)) return;
        std::vector<MediaItem> v;std::string e;bool ok=RouteRequest(s).run([&](const std::string &base){return JellyfinApi::getSeasons(base,s.accessToken,s.userId,s.deviceId,id,v,e,&m_fetchCancelled);},e);
        // Persist network results on the worker too: save() fsyncs the catalog.
        if(ok&&!m_fetchCancelled.load(std::memory_order_acquire)) OfflineCatalog::storeSeasons(catalogPath,series,v,nullptr);
        std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=ok;m_fetchSeasons=std::move(v);m_fetchError=e;m_fetchDone=true;
    });
}

void SeriesScreen::tryLoadSeriesArtwork()
{
    if (m_seriesArtworkAttempted)
        return;
    m_seriesArtworkAttempted = true;

    // Look for Primary image tag using find(), not operator[]
    auto it = m_series.imageTags.find("Primary");
    if (it == m_series.imageTags.end() || it->second.empty()) {
        printf("[SeriesScreen] Artwork: no Primary tag\n");
        return;
    }

    const std::string &tag = it->second;
    constexpr int w = SHOW_W;   // 160
    constexpr int h = SHOW_H;   // 240

    printf("[SeriesScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           m_series.id.c_str(), tag.c_str(), w, h);

    queueArtwork("series:" + m_series.id + ":" + tag, m_series, w, h, true);
}

void SeriesScreen::queueArtwork(const std::string &key, const MediaItem &item, int width, int height, bool series)
{
    {
        std::lock_guard<std::mutex> g(m_artworkMutex);
        m_artworkJobs.push_back({key, item, width, height, series});
        if (!m_artworkThread.joinable()) m_artworkThread=std::thread(&SeriesScreen::artworkWorkerLoop, this);
    }
    m_artworkCv.notify_one();
}

void SeriesScreen::artworkWorkerLoop()
{
    for (;;) {
        ArtworkJob job;
        { std::unique_lock<std::mutex> l(m_artworkMutex); m_artworkCv.wait(l,[&]{return m_artworkStop || !m_artworkJobs.empty();}); if(m_artworkStop)return; job=std::move(m_artworkJobs.front());m_artworkJobs.erase(m_artworkJobs.begin()); }
        auto tag=job.item.imageTags.find("Primary"); if(tag==job.item.imageTags.end()) continue;
        std::vector<unsigned char> data;
        if(ImageCache::isCached(job.item.id,ImageType::Primary,tag->second,job.width,job.height)) data=ImageCache::readCached(job.item.id,ImageType::Primary,tag->second,job.width,job.height);
        if(data.empty() && !m_artworkCancelled.load(std::memory_order_acquire)) { HttpClient c;c.setTimeoutSec(8);BinaryHttpResponse r;std::string e;if(RouteRequest(m_session).run([&](const std::string &base){return c.getBinary(buildImageUrl(base,job.item.id,ImageType::Primary,tag->second,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),r,e,512*1024,&m_artworkCancelled)&&r.ok();},e)){data=std::move(r.data);ImageCache::writeToCache(job.item.id,ImageType::Primary,tag->second,job.width,job.height,data.data(),data.size());} }
        DecodedImage image; if(!data.empty()&&!m_artworkCancelled.load(std::memory_order_acquire)) image=ImageDecoder::decodeJpeg(data.data(),data.size());
        {std::lock_guard<std::mutex>g(m_artworkMutex);if(!m_artworkStop)m_artworkCompleted[job.key]=std::move(image);}
    }
}

// -------------------------------------------------------------------
// Season poster artwork — load at most one per update() cycle
// -------------------------------------------------------------------

void SeriesScreen::tryLoadOneVisibleSeasonArtwork()
{
    if (m_loadState != LoadState::Ready)
        return;

    int totalSeasons = (int)m_seasons.size();

    for (int vis = 0; vis < GRID_VISIBLE; ++vis) {
        int gridRow = vis / GRID_COLS;
        int gridCol = vis % GRID_COLS;
        int itemIdx = (m_gridScroll + gridRow) * GRID_COLS + gridCol;
        if (itemIdx >= totalSeasons) break;

        const MediaItem &season = m_seasons[itemIdx];

        std::string key = seasonArtworkKey(season);
        if (key.empty())
            continue;

        // Skip if already attempted (successful or not)
        if (m_seasonArtwork.count(key) > 0)
            continue;

        // Mark attempted immediately to prevent retry
        m_seasonArtwork[key] = DecodedImage{};  // empty = attempted, not yet decoded

        auto tagIt = season.imageTags.find("Primary");
        const std::string &tag = tagIt->second;

        printf("[SeriesScreen] SeasonArtwork: loading %s tag=%s (%dx%d)\n",
               season.id.c_str(), tag.c_str(), POSTER_W, POSTER_H);

        queueArtwork(key, season, POSTER_W, POSTER_H, false);
        return;
    }
}

bool SeriesScreen::handleAction(Action action)
{
    if (m_loadState == LoadState::Loading) {
        if (action == Action::Back) {
            m_stack->pop();
            return true;
        }
        return false;
    }

    if (m_loadState == LoadState::Error) {
        switch (action) {
        case Action::Back:
            m_stack->pop();
            return true;
        case Action::Confirm:
            fetchSeasons();
            return true;
        default:
            return false;
        }
    }

    // Ready — 2-column grid navigation
    int total = (int)m_seasons.size();
    int col   = m_selectedSeason % GRID_COLS;

    if (m_confirmDownload) {
        if(action==Action::Back){m_confirmDownload=false;return true;}
        if(action==Action::Confirm && m_downloads && m_planId){auto p=m_downloads->planSnapshot(m_planId);if(p.state==DownloadPlanState::Ready&&p.plan.canFit)m_downloads->enqueue(p.plan.items);m_confirmDownload=false;}
        return true;
    }
    // Y downloads the highlighted season; X expands and downloads the series.
    // Both requests are fully expanded and preflighted by DownloadManager.
    if ((action==Action::ActionsMenu || action==Action::Search) && m_downloads && total>0) {
        bool whole=action==Action::Search;
        if(m_planId && m_planWholeSeries==whole && m_downloads->planSnapshot(m_planId).state==DownloadPlanState::Ready) m_confirmDownload=true;
        else {m_planWholeSeries=whole;m_planId=whole?m_downloads->requestSeriesPlan(m_series):m_downloads->requestSeasonPlan(m_series,m_seasons[m_selectedSeason]);}
        return true;
    }

    switch (action) {
    case Action::Left:
        if (col > 0 && m_selectedSeason > 0) {
            m_selectedSeason--;
            clampGridScroll();
        }
        return true;

    case Action::Right:
        if (col < GRID_COLS - 1 && m_selectedSeason + 1 < total) {
            m_selectedSeason++;
            clampGridScroll();
        }
        return true;

    case Action::Up: {
        // Move up one grid row (same column)
        int newIdx = m_selectedSeason - GRID_COLS;
        if (newIdx >= 0) {
            m_selectedSeason = newIdx;
            clampGridScroll();
        }
        return true;
    }

    case Action::Down: {
        // Move down one grid row (same column if possible)
        int newIdx = m_selectedSeason + GRID_COLS;
        if (newIdx < total) {
            m_selectedSeason = newIdx;
            clampGridScroll();
        } else if (m_selectedSeason + 1 < total) {
            // Overshoot: land on the last item instead
            m_selectedSeason = total - 1;
            clampGridScroll();
        }
        return true;
    }

    case Action::Confirm: {
        const MediaItem &season = m_seasons[m_selectedSeason];
        printf("[SeriesScreen] Select season: %s index=%d\n",
               season.title.c_str(), season.indexNumber);
        m_stack->push(std::make_unique<EpisodeBrowserScreen>(
            m_session, m_series, season, "", m_downloads, m_offline));
        return true;
    }

    case Action::Back:
        m_stack->pop();
        return true;

    case Action::PrevTab: {
        m_overviewScroll -= 3;
        if (m_overviewScroll < 0) m_overviewScroll = 0;
        return true;
    }

    case Action::NextTab: {
        auto lines = wrapOverview(m_series.overview.c_str(), META_WRAP);
        int overviewStartY = META_Y + BitmapFont::GLYPH_H + 2;
        if (m_series.year > 0 || !m_series.genre.empty())
            overviewStartY += BitmapFont::GLYPH_H + 2;
        int vis = ((FB_H - BOTTOM_H) - overviewStartY) / BitmapFont::GLYPH_H;
        if (vis < 1) vis = 1;
        int maxS = (int)lines.size() - vis;
        if (maxS < 0) maxS = 0;
        m_overviewScroll += 3;
        if (m_overviewScroll > maxS) m_overviewScroll = maxS;
        return true;
    }

    default:
        return false;
    }
}

void SeriesScreen::update(Uint32 /*dt*/)
{
    std::vector<MediaItem> cached;
    bool cachedDone=false;
    {std::lock_guard<std::mutex>g(m_fetchMutex);if(m_cachedSeasonsDone){cached=std::move(m_cachedSeasons);m_cachedSeasonsDone=false;cachedDone=true;}}
    if(cachedDone) {
        UiDiagnostics::Scope scope("SeriesScreen::publishCachedSeasons");
        m_seasons=std::move(cached);
        // Offline completion and a usable online cache are immediately
        // interactive; an empty online cache continues showing the loader
        // while the network request is in flight.
        if(m_offline || !m_seasons.empty()) m_loadState=LoadState::Ready;
    }
    bool fetchDone; {std::lock_guard<std::mutex>g(m_fetchMutex);fetchDone=m_fetchDone;}
    if(fetchDone){UiDiagnostics::Scope scope("SeriesScreen::publishSeasonResult");std::vector<MediaItem> fresh;std::string err;bool ok;{std::lock_guard<std::mutex>g(m_fetchMutex);ok=m_fetchOk;fresh=std::move(m_fetchSeasons);err=m_fetchError;m_fetchDone=false;}if(m_fetchThread.joinable())m_fetchThread.join();if(ok){if(!m_offline)m_seasons=replaceSeasonsKeepingSelection(m_seasons,std::move(fresh),m_selectedSeason);m_loadState=LoadState::Ready;}else if(m_seasons.empty()){m_loadState=LoadState::Error;m_error=err;}}
    {
        UiDiagnostics::Scope scope("SeriesScreen::publishArtworkResult");
        std::lock_guard<std::mutex> g(m_artworkMutex);
        for (auto &entry : m_artworkCompleted) {
            if (entry.first.rfind("series:", 0) == 0) {
                m_seriesArtwork = std::move(entry.second);
                freePreparedArtwork(m_seriesArtworkSurface);
                m_seriesArtworkSurface = prepareArtworkSurface(m_seriesArtwork, SHOW_X, SHOW_Y, SHOW_W, SHOW_H);
            } else {
                m_seasonArtwork[entry.first] = std::move(entry.second);
                PreparedArtwork &prepared = m_seasonArtworkSurfaces[entry.first];
                freePreparedArtwork(prepared);
                prepared = prepareArtworkSurface(m_seasonArtwork[entry.first], 0, 0, POSTER_W, POSTER_H);
            }
        }
        m_artworkCompleted.clear();
    }
    {UiDiagnostics::Scope scope("SeriesScreen::queueVisibleArtwork");tryLoadOneVisibleSeasonArtwork();}
}

// -------------------------------------------------------------------
// Grid navigation helpers
// -------------------------------------------------------------------

/// Total grid rows needed to hold all seasons.
static int gridRowCount(int totalSeasons)
{
    return (totalSeasons + GRID_COLS - 1) / GRID_COLS;
}

/// Clamp gridScroll so the selected season's row is visible.
void SeriesScreen::clampGridScroll()
{
    if (m_seasons.empty()) { m_gridScroll = 0; return; }
    int selRow = m_selectedSeason / GRID_COLS;
    int totalRows = gridRowCount((int)m_seasons.size());

    if (selRow < m_gridScroll)
        m_gridScroll = selRow;
    else if (selRow >= m_gridScroll + GRID_ROWS)
        m_gridScroll = selRow - GRID_ROWS + 1;

    if (m_gridScroll < 0) m_gridScroll = 0;
    int maxScroll = totalRows - GRID_ROWS;
    if (maxScroll < 0) maxScroll = 0;
    if (m_gridScroll > maxScroll) m_gridScroll = maxScroll;
}

// -------------------------------------------------------------------
// Rendering helpers
// -------------------------------------------------------------------

static void renderBottomHints(SDL_Surface *fb, const char *hint)
{
    int y = FB_H - BOTTOM_H;
    BitmapFont::fillRect(fb, 0, y, FB_W, BOTTOM_H,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3, 255);

    BitmapFont::drawString(fb, 8, y + 2, hint,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3);
}

SeriesScreen::PreparedArtwork SeriesScreen::prepareArtworkSurface(const DecodedImage &image,
                                                                    int boxX, int boxY, int boxW, int boxH)
{
    PreparedArtwork prepared;
    if (image.empty()) return prepared;

    float imgAspect = (float)image.width / (float)image.height;
    float boxAspect = (float)boxW / (float)boxH;
    int drawW, drawH;
    if (imgAspect > boxAspect) {
        drawW = boxW;
        drawH = (int)(boxW / imgAspect + 0.5f);
        if (drawH > boxH) drawH = boxH;
    } else {
        drawH = boxH;
        drawW = (int)(boxH * imgAspect + 0.5f);
        if (drawW > boxW) drawW = boxW;
    }

    SDL_Surface *source = SDL_CreateRGBSurfaceFrom(
        (void *)image.pixels.data(), image.width, image.height, 32, image.width * 4,
        0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    SDL_Surface *destination = SDL_CreateRGBSurface(
        0, drawW, drawH, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (source && destination) {
        SDL_Rect sourceRect = {0, 0, image.width, image.height};
        SDL_Rect destinationRect = {0, 0, drawW, drawH};
        SDL_BlitScaled(source, &sourceRect, destination, &destinationRect);
        prepared.surface = destination;
        prepared.x = boxX + (boxW - drawW) / 2;
        prepared.y = boxY + (boxH - drawH) / 2;
    } else if (destination) {
        SDL_FreeSurface(destination);
    }
    if (source) SDL_FreeSurface(source);
    return prepared;
}

void SeriesScreen::freePreparedArtwork(PreparedArtwork &artwork)
{
    if (artwork.surface) SDL_FreeSurface(artwork.surface);
    artwork = {};
}

/// Draw a season poster card (placeholder colour + optional artwork + title overlay + border).
void SeriesScreen::drawSeasonPoster(SDL_Surface *fb, int x, int y, int w, int h,
                                    const MediaItem &season, bool selected,
                                    const PreparedArtwork *artwork)
{
    // Placeholder colour fill (always drawn as fallback)
    BitmapFont::fillRect(fb, x, y, w, h,
        season.artR, season.artG, season.artB, 255);

    if (artwork && artwork->surface) {
        SDL_Rect dstRect = {x + artwork->x, y + artwork->y, 0, 0};
        SDL_BlitSurface(artwork->surface, nullptr, fb, &dstRect);
    }

    // Dark overlay along the bottom for the title
    int overlayY = y + h - OVERLAY_H;
    BitmapFont::fillRect(fb, x, overlayY, w, OVERLAY_H, 0, 0, 0, 160);

    // Season label — compact fallback if full title doesn't fit
    int maxChars = (w - 4) / BitmapFont::GLYPH_W;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", season.title.c_str());
    if ((int)std::strlen(buf) > maxChars && season.indexNumber > 0) {
        std::snprintf(buf, sizeof(buf), "S%d", season.indexNumber);
    }
    if ((int)std::strlen(buf) > maxChars) {
        if (maxChars > 2) {
            buf[maxChars - 1] = '.';
            buf[maxChars - 2] = '.';
        }
        buf[maxChars] = '\0';
    }
    BitmapFont::drawString(fb, x + 2, overlayY + 1, buf,
        255, 255, 255, 0, 0, 0);

    // Selection border — yellow double-border (HomeScreen style)
    if (selected) {
        BitmapFont::drawRect(fb, x - 2, y - 2, w + 4, h + 4,
            255, 220, 40);   // outer
        BitmapFont::drawRect(fb, x - 1, y - 1, w + 2, h + 2,
            255, 255, 120);  // inner
    } else {
        BitmapFont::drawRect(fb, x, y, w, h,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
    }
}

// -------------------------------------------------------------------
// Main render
// -------------------------------------------------------------------

void SeriesScreen::render(SDL_Surface *fb)
{
    // --- Loading state ---
    if (m_loadState == LoadState::Loading) {
        const char *msg = "Loading seasons...";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        int my = FB_H / 3;
        BitmapFont::drawString(fb, mx, my, msg,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "A=Retry  B=Back");
        return;
    }

    // --- Error state ---
    if (m_loadState == LoadState::Error) {
        const char *header = "Failed to load seasons";
        int hx = (FB_W - (int)std::strlen(header) * BitmapFont::GLYPH_W) / 2;
        int hy = FB_H / 3;
        BitmapFont::drawString(fb, hx, hy, header,
            Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        char errBuf[128];
        std::snprintf(errBuf, sizeof(errBuf), "%s", m_error.c_str());
        int maxCols = (FB_W - 32) / BitmapFont::GLYPH_W;
        if ((int)std::strlen(errBuf) > maxCols) errBuf[maxCols] = '\0';
        int ex = (FB_W - (int)std::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
        int ey = hy + BitmapFont::GLYPH_H + 8;
        BitmapFont::drawString(fb, ex, ey, errBuf,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "A=Retry  B=Back");
        return;
    }

    // =================================================================
    // Ready state — full two-panel layout
    // =================================================================

    // 1. Top-left heading
    BitmapFont::drawString(fb, HEAD_X, HEAD_Y, "SEASONS",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    if (m_downloads && m_planId) {
        auto p=m_downloads->planSnapshot(m_planId); std::string status;
        const char *what=m_planWholeSeries?"Series":"Season";
        if(m_confirmDownload) status=std::string("Download ")+what+"?";
        else if(p.state==DownloadPlanState::Planning) status=p.plan.sizeKnown?std::to_string(p.itemCount)+" episodes  ~"+formatBytes(p.plan.additionalRequiredBytes)+" estimated":std::string("Planning ")+what+" download...";
        else if(p.state==DownloadPlanState::Ready) status=std::to_string(p.itemCount)+" episodes  ~"+formatBytes(p.plan.additionalRequiredBytes)+" needed  "+formatBytes(p.plan.usableFreeBytes)+" free";
        else if(p.state==DownloadPlanState::Error) status=p.plan.error;
        if(!status.empty()) BitmapFont::drawString(fb,HEAD_X,HEAD_Y+14,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
        if(m_confirmDownload) BitmapFont::drawString(fb,HEAD_X,HEAD_Y+28,"A=Confirm  B=Cancel",Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
    }

    // 2. Season poster grid (left side, 2 columns × 3 rows)
    int totalSeasons = (int)m_seasons.size();
    for (int vis = 0; vis < GRID_VISIBLE; ++vis) {
        int gridRow = vis / GRID_COLS;
        int gridCol = vis % GRID_COLS;
        int itemIdx = (m_gridScroll + gridRow) * GRID_COLS + gridCol;
        if (itemIdx >= totalSeasons) break;

        int px = COL_X[gridCol];
        int py = GRID_TOP_Y + gridRow * GRID_ROW_H;
        bool sel = (itemIdx == m_selectedSeason);

        // Look up prepared season artwork (read-only, no mutation during render)
        const PreparedArtwork *artPtr = nullptr;
        std::string key = seasonArtworkKey(m_seasons[itemIdx]);
        if (!key.empty()) {
            auto it = m_seasonArtworkSurfaces.find(key);
            if (it != m_seasonArtworkSurfaces.end() && it->second.surface)
                artPtr = &it->second;
        }

        drawSeasonPoster(fb, px, py, POSTER_W, POSTER_H,
                         m_seasons[itemIdx], sel, artPtr);
    }

    // 3. Right-side show poster placeholder
    BitmapFont::fillRect(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H,
        m_series.artR, m_series.artG, m_series.artB, 255);

    if (m_seriesArtworkSurface.surface) {
        SDL_Rect dstRect = {m_seriesArtworkSurface.x, m_seriesArtworkSurface.y, 0, 0};
        SDL_BlitSurface(m_seriesArtworkSurface.surface, nullptr, fb, &dstRect);
    }

    // Poster border (drawn after artwork)
    BitmapFont::drawRect(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);

    // 4. Metadata under the show poster
    int my = META_Y;

    // Series title
    BitmapFont::drawString(fb, META_X, my, m_series.title.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    my += BitmapFont::GLYPH_H + 2;

    // Year and first genre
    if (m_series.year > 0 || !m_series.genre.empty()) {
        char meta[128];
        if (m_series.year > 0 && !m_series.genre.empty())
            std::snprintf(meta, sizeof(meta), "%d  |  %s",
                          m_series.year, m_series.genre.c_str());
        else if (m_series.year > 0)
            std::snprintf(meta, sizeof(meta), "%d", m_series.year);
        else
            std::snprintf(meta, sizeof(meta), "%s", m_series.genre.c_str());
        BitmapFont::drawString(fb, META_X, my, meta,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        my += BitmapFont::GLYPH_H + 2;
    }

    // Overview / bio (word-wrapped, scrollable)
    bool overviewScrollable = false;
    if (!m_series.overview.empty()) {
        auto lines = wrapOverview(m_series.overview.c_str(), META_WRAP);
        int overviewEndY = FB_H - BOTTOM_H;
        int visibleLines = (overviewEndY - my) / BitmapFont::GLYPH_H;
        if (visibleLines < 1) visibleLines = 1;

        int maxScroll = (int)lines.size() - visibleLines;
        if (maxScroll < 0) maxScroll = 0;
        if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        if (m_overviewScroll < 0) m_overviewScroll = 0;

        overviewScrollable = (maxScroll > 0);

        int drawY = my;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + visibleLines && i < (int)lines.size(); ++i) {
            BitmapFont::drawString(fb, META_X, drawY, lines[i].c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
            drawY += BitmapFont::GLYPH_H;
        }
    }

    // 5. Bottom hint bar
    renderBottomHints(fb, overviewScrollable
        ? "A=Open B=Back Y=Season X=Series L/R=Bio" : "A=Open B=Back Y=Season X=Series");
}

} // namespace miyoofin
