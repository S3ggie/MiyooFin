#include "SeriesScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
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

SeriesScreen::SeriesScreen(const Session &session, const MediaItem &series, std::shared_ptr<DownloadManager> downloads, bool offline)
    : m_session(session)
    , m_series(series)
    , m_downloads(std::move(downloads))
    , m_offline(offline)
{
}

void SeriesScreen::enter()
{
    printf("[SeriesScreen] enter series=%s\n", m_series.title.c_str());
    if (m_seasons.empty()) {
        OfflineCatalogSnapshot catalog; OfflineCatalog::load(OfflineCatalog::cachePath("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),catalog,nullptr); LibrarySnapshot library; OfflineLibraryProjection projection(library,catalog,m_downloads?m_downloads->snapshot():DownloadSnapshot{}); m_seasons=m_offline?projection.seasons(m_series.id):OfflineCatalog::seasons(OfflineCatalog::cachePath("cache", LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_series.id);
        m_loadState=LoadState::Ready;
        if(!m_offline) fetchSeasons();
    }
    tryLoadSeriesArtwork();
}
SeriesScreen::~SeriesScreen(){ leave(); if(m_fetchThread.joinable())m_fetchThread.join(); if(m_artworkThread.joinable())m_artworkThread.join(); }

void SeriesScreen::leave()
{
    printf("[SeriesScreen] leave series=%s\n", m_series.title.c_str());
    m_fetchCancelled.store(true, std::memory_order_release);
    m_artworkCancelled.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> g(m_artworkMutex); m_artworkStop=true; }
    m_artworkCv.notify_one();
}

void SeriesScreen::fetchSeasons()
{
    if(m_fetchThread.joinable()) { std::lock_guard<std::mutex>g(m_fetchMutex); if(!m_fetchDone)return; m_fetchThread.join(); }
    if(m_seasons.empty()) m_loadState = LoadState::Loading;
    m_error.clear();
    {std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchDone=false;} m_fetchCancelled.store(false, std::memory_order_release); Session s=m_session; std::string id=m_series.id;
    m_fetchThread=std::thread([this,s,id](){std::vector<MediaItem> v;std::string e;bool ok=JellyfinApi::getSeasons(s.serverUrl,s.accessToken,s.userId,s.deviceId,id,v,e,&m_fetchCancelled);std::lock_guard<std::mutex>g(m_fetchMutex);m_fetchOk=ok;m_fetchSeasons=std::move(v);m_fetchError=e;m_fetchDone=true;});
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
        if(data.empty() && !m_artworkCancelled.load(std::memory_order_acquire)) { HttpClient c;c.setTimeoutSec(8);BinaryHttpResponse r;std::string e;if(c.getBinary(buildImageUrl(m_session.serverUrl,job.item.id,ImageType::Primary,tag->second,job.width,job.height),JellyfinApi::buildAuthHeaders(m_session.accessToken,m_session.deviceId),r,e,512*1024,&m_artworkCancelled)&&r.ok()){data=std::move(r.data);ImageCache::writeToCache(job.item.id,ImageType::Primary,tag->second,job.width,job.height,data.data(),data.size());} }
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
    bool fetchDone; {std::lock_guard<std::mutex>g(m_fetchMutex);fetchDone=m_fetchDone;}
    if(fetchDone){std::vector<MediaItem> fresh;std::string err;bool ok;{std::lock_guard<std::mutex>g(m_fetchMutex);ok=m_fetchOk;fresh=std::move(m_fetchSeasons);err=m_fetchError;m_fetchDone=false;}if(m_fetchThread.joinable())m_fetchThread.join();if(ok){std::string selected=m_seasons.empty()?"":m_seasons[m_selectedSeason].id;m_seasons=std::move(fresh);int n=0;for(;n<(int)m_seasons.size()&&m_seasons[n].id!=selected;n++);m_selectedSeason=n<(int)m_seasons.size()?n:0;m_loadState=LoadState::Ready;OfflineCatalog::storeSeasons(OfflineCatalog::cachePath("cache",LibraryCache::scopeKey(m_session.serverUrl,m_session.userId)),m_series,m_seasons,nullptr);}else if(m_seasons.empty()){m_loadState=LoadState::Error;m_error=err;}}
    {std::lock_guard<std::mutex>g(m_artworkMutex);for(auto &entry:m_artworkCompleted){if(entry.first.rfind("series:",0)==0)m_seriesArtwork=std::move(entry.second);else m_seasonArtwork[entry.first]=std::move(entry.second);}m_artworkCompleted.clear();}
    tryLoadOneVisibleSeasonArtwork();
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

/// Draw a season poster card (placeholder colour + optional artwork + title overlay + border).
static void drawSeasonPoster(SDL_Surface *fb, int x, int y, int w, int h,
                              const MediaItem &season, bool selected,
                              const DecodedImage *artwork = nullptr)
{
    // Placeholder colour fill (always drawn as fallback)
    BitmapFont::fillRect(fb, x, y, w, h,
        season.artR, season.artG, season.artB, 255);

    // Render decoded artwork if available, aspect-fit centred
    if (artwork && !artwork->empty()) {
        int imgW = artwork->width;
        int imgH = artwork->height;
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
            (void *)artwork->pixels.data(),
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

        // Look up decoded season artwork (read-only, no mutation during render)
        const DecodedImage *artPtr = nullptr;
        std::string key = seasonArtworkKey(m_seasons[itemIdx]);
        if (!key.empty()) {
            auto it = m_seasonArtwork.find(key);
            if (it != m_seasonArtwork.end() && !it->second.empty())
                artPtr = &it->second;
        }

        drawSeasonPoster(fb, px, py, POSTER_W, POSTER_H,
                         m_seasons[itemIdx], sel, artPtr);
    }

    // 3. Right-side show poster placeholder
    BitmapFont::fillRect(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H,
        m_series.artR, m_series.artG, m_series.artB, 255);

    // Render decoded series artwork if available, aspect-fit centred
    if (!m_seriesArtwork.empty()) {
        int imgW = m_seriesArtwork.width;
        int imgH = m_seriesArtwork.height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)SHOW_W / (float)SHOW_H;
        int drawW, drawH;
        if (imgAspect > boxAspect) {
            // Wider than box — fit to width
            drawW = SHOW_W;
            drawH = (int)(SHOW_W / imgAspect + 0.5f);
            if (drawH > SHOW_H) drawH = SHOW_H;
        } else {
            // Taller than box — fit to height
            drawH = SHOW_H;
            drawW = (int)(SHOW_H * imgAspect + 0.5f);
            if (drawW > SHOW_W) drawW = SHOW_W;
        }
        int drawX = SHOW_X + (SHOW_W - drawW) / 2;
        int drawY = SHOW_Y + (SHOW_H - drawH) / 2;

        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_seriesArtwork.pixels.data(),
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
