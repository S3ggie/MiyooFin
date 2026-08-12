#include "MovieDetailsScreen.hpp"
#include "../../download/DownloadSupport.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

// -------------------------------------------------------------------
// Layout constants — 640x480 framebuffer, two-panel design
// -------------------------------------------------------------------
static constexpr int FB_W           = 640;
static constexpr int FB_H           = 480;
static constexpr int BOTTOM_H       = 18;

// Left panel — large movie poster
static constexpr int POSTER_X       = 28;
static constexpr int POSTER_Y       = 48;
static constexpr int POSTER_W       = 160;
static constexpr int POSTER_H       = 240;

// Right panel
static constexpr int RIGHT_X        = 215;
static constexpr int RIGHT_TOP_Y    = 48;
static constexpr int META_WRAP      = 34;

// Action buttons
static constexpr int BTN_W          = 80;
static constexpr int BTN_H          = 20;
static constexpr int BTN_Y          = FB_H - BOTTOM_H - BTN_H - 6;
static constexpr int BTN_PLAY_X     = 260;
static constexpr int BTN_DL_X       = 360;

// Yellow double-border focus colours (same as EpisodeBrowserScreen)
static constexpr Uint8 FOCUS_OR = 255, FOCUS_OG = 220, FOCUS_OB = 40;
static constexpr Uint8 FOCUS_IR = 255, FOCUS_IG = 255, FOCUS_IB = 120;

// Scroll lines per L/R press
static constexpr int SCROLL_STEP = 3;

// -------------------------------------------------------------------
// Word-wrap helper (same algorithm as SeriesScreen / EpisodeBrowserScreen)
// -------------------------------------------------------------------
static std::vector<std::string> wrapText(const char *text, int wrapCols)
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

// -------------------------------------------------------------------
// Bottom hint bar renderer (matches SeriesScreen)
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

// -------------------------------------------------------------------
// Constructor
// -------------------------------------------------------------------
MovieDetailsScreen::MovieDetailsScreen(const Session &session,
                                       const MediaItem &movie, std::shared_ptr<DownloadManager> downloads,
                                       std::shared_ptr<const DecodedImage> gridArtwork)
    : m_session(session)
    , m_movie(movie)
    , m_downloads(std::move(downloads))
    , m_gridArtwork(std::move(gridArtwork))
{
    if(m_gridArtwork&&!m_gridArtwork->empty()){
        m_gridArtworkSurface=SDL_CreateRGBSurfaceFrom(
            (void*)m_gridArtwork->pixels.data(),m_gridArtwork->width,m_gridArtwork->height,
            32,m_gridArtwork->width*4,
            0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    }
}

// -------------------------------------------------------------------
// enter — attempt artwork load
// -------------------------------------------------------------------
void MovieDetailsScreen::enter()
{
    UiDiagnostics::Scope scope("MovieDetailsScreen::enter");
    printf("[MovieDetailsScreen] enter: %s\n", m_movie.title.c_str());
    if(!m_prepareStarted){
        uiDiagnostics().event("MovieDetailsScreen: cached item; no LibraryCache/OfflineCatalog/offline projection on open");
        UiDiagnostics::Scope createScope("MovieDetailsScreen::owned worker creation");
        m_prepareStarted=true;
        m_prepareThread=std::thread(&MovieDetailsScreen::prepareWorker,this);
    }
}

MovieDetailsScreen::~MovieDetailsScreen()
{
    leave();
    if(m_prepareThread.joinable()){
        // Popped Movie screens are destroyed by ScreenStack's retirement
        // worker, so this join can never delay SDL input/render.
        UiDiagnostics::Scope scope("MovieDetailsScreen::owned worker join",false);
        m_prepareThread.join();
    }
}

// -------------------------------------------------------------------
// leave
// -------------------------------------------------------------------
void MovieDetailsScreen::leave()
{
    if(m_shutdownSignalled.exchange(true,std::memory_order_acq_rel))return;
    UiDiagnostics::Scope scope("MovieDetailsScreen::worker cancellation");
    m_prepareCancelled.store(true,std::memory_order_release);
    if(m_movieArtworkSurface){
        SDL_FreeSurface(m_movieArtworkSurface);
        m_movieArtworkSurface=nullptr;
    }
    if(m_gridArtworkSurface){
        SDL_FreeSurface(m_gridArtworkSurface);
        m_gridArtworkSurface=nullptr;
    }
    m_gridArtwork.reset();
}

// -------------------------------------------------------------------
// handleAction
// -------------------------------------------------------------------
bool MovieDetailsScreen::handleAction(Action action)
{
    switch (action) {
    case Action::Back:
        m_stack->pop();
        return true;

    // Overview scroll
    case Action::PrevTab: {   // L — scroll bio UP
        if (m_overviewScroll > 0)
            m_overviewScroll -= SCROLL_STEP;
        if (m_overviewScroll < 0) m_overviewScroll = 0;
        return true;
    }
    case Action::NextTab: {   // R — scroll bio DOWN
        m_overviewScroll += SCROLL_STEP;
        return true;
    }

    // D-pad for action buttons
    case Action::Left:
        if (m_actionBtn == ActionButton::Download)
            m_actionBtn = ActionButton::Play;
        return true;
    case Action::Right:
        if (m_actionBtn == ActionButton::Play)
            m_actionBtn = ActionButton::Download;
        return true;

    // Confirm
    case Action::Confirm:
        if (m_confirmDownload) { if (m_downloads && m_planId && m_planSnapshot.state==DownloadPlanState::Ready&&m_planSnapshot.plan.canFit) m_downloads->enqueue(m_planSnapshot.plan.items); m_confirmDownload=false; return true; }
        if (m_actionBtn == ActionButton::Play) {
            printf("[MovieDetailsScreen] Play selected: %s\n",
                   m_movie.title.c_str());
            std::string error;
            PlaybackSource source=m_downloads?resolvePlayback(m_movie,*m_downloads):PlaybackSource::Jellyfin; if(source==PlaybackSource::UnavailableOffline)return true;
            const std::string mode=source==PlaybackSource::Local?"local":"jellyfin";
            if (PlaybackRequest::writeWithSourceTo(PlaybackRequest::defaultPath(),m_movie.id, "movie",
                                       m_movie.playbackPositionTicks, mode, mode=="local"?m_downloads->scope():"", error)) {
                m_playbackResultPending = true;
                m_playbackResultDelayUpdates = 1;
                printf("[MovieDetailsScreen] Playback request written, "
                       "requesting external playback\n");
                m_stack->requestExternalPlayback();
            } else {
                printf("[MovieDetailsScreen] Playback request failed: %s\n",
                       error.c_str());
            }
        } else {
            if(m_downloads && m_planId && m_planSnapshot.state==DownloadPlanState::Ready) m_confirmDownload=true;
        }
        return true;

    default:
        return false;
    }
}

// -------------------------------------------------------------------
// update
// -------------------------------------------------------------------
void MovieDetailsScreen::update(Uint32 /*dt*/)
{
    {
        UiDiagnostics::Scope scope("MovieDetailsScreen::publish async preparation");
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(m_preparedPlanReady){
            m_planId=m_preparedPlanId;
            m_preparedPlanReady=false;
        }
        // Keep the first frame cheap: it uses the handed-off grid image when
        // available, otherwise the placeholder, even if the worker wins.
        if(m_preparedArtworkReady&&!m_firstRender){
            m_preparedArtworkReady=false;
            if(!m_preparedArtwork.empty()){
                m_movieArtwork=std::move(m_preparedArtwork);
                UiDiagnostics::Scope imageScope("MovieDetailsScreen::publish image surface preparation");
                m_movieArtworkSurface=SDL_CreateRGBSurfaceFrom(
                    (void*)m_movieArtwork.pixels.data(),m_movieArtwork.width,m_movieArtwork.height,
                    32,m_movieArtwork.width*4,
                    0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
                if(m_movieArtworkSurface){
                    if(m_gridArtworkSurface){
                        SDL_FreeSurface(m_gridArtworkSurface);
                        m_gridArtworkSurface=nullptr;
                    }
                    m_gridArtwork.reset();
                }
            }
        }
        if(m_preparedOverviewReady){
            m_overviewLines=std::move(m_preparedOverviewLines);
            m_preparedOverviewReady=false;
        }
    }
    if(m_downloads&&m_planId){
        UiDiagnostics::Scope scope("MovieDetailsScreen::publish playback/download state");
        DownloadPlanSnapshot snapshot;
        if(m_downloads->tryPlanSnapshot(m_planId,snapshot))m_planSnapshot=std::move(snapshot);
    }
    if (PlaybackRequest::advanceResultConsumption(
            m_playbackResultPending, m_playbackResultDelayUpdates)) {
        std::int64_t resultTicks = 0;
        std::string error;
        if (PlaybackRequest::consumeResult(m_movie.id, resultTicks, error)) {
            m_movie.playbackPositionTicks = resultTicks;
            printf("[MovieDetailsScreen] Playback position updated: %lld\n",
                   (long long)resultTicks);
        }
    }
}

// -------------------------------------------------------------------
// prepareWorker — all potentially blocking first-state work
// -------------------------------------------------------------------
void MovieDetailsScreen::prepareWorker()
{
    // The selected MediaItem already contains the detail metadata. There is
    // deliberately no LibraryCache/OfflineCatalog read or offline projection
    // in this open path; downloaded-only Movies keep the item projected by
    // HomeScreen and local playback is still resolved when Play is pressed.
    std::vector<std::string> overviewLines;
    {
        UiDiagnostics::Scope scope("MovieDetailsScreen::metadata preparation",false);
        overviewLines=wrapText(m_movie.overview.c_str(),META_WRAP);
    }
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedOverviewLines=std::move(overviewLines);
            m_preparedOverviewReady=true;
        }
    }

    uiDiagnostics().setWorker("artwork","movie download state");
    if(m_downloads&&!m_prepareCancelled.load(std::memory_order_acquire)){
        std::uint64_t planId=0;
        {
            // requestPlan includes DownloadManager mutex acquisition, its
            // snapshot/local-state estimate, and filesystem free-space lookup.
            UiDiagnostics::Scope scope("MovieDetailsScreen::DownloadManager snapshot/free-space preparation",false);
            planId=m_downloads->requestPlan({m_movie});
        }
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedPlanId=planId;
            m_preparedPlanReady=true;
        }
    }

    DecodedImage artwork;
    if(!m_prepareCancelled.load(std::memory_order_acquire))artwork=loadMovieArtwork();
    {
        std::lock_guard<std::mutex> lock(m_prepareMutex);
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            m_preparedArtwork=std::move(artwork);
            m_preparedArtworkReady=true;
        }
    }
    uiDiagnostics().setWorker("artwork","idle");
}

// -------------------------------------------------------------------
// loadMovieArtwork — worker-only cache/network/decode path
// -------------------------------------------------------------------
DecodedImage MovieDetailsScreen::loadMovieArtwork()
{

    auto it = m_movie.imageTags.find("Primary");
    if (it == m_movie.imageTags.end() || it->second.empty()) {
        printf("[MovieDetailsScreen] No Primary artwork tag for %s\n",
               m_movie.title.c_str());
        return {};
    }

    const std::string &tag = it->second;
    printf("[MovieDetailsScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           m_movie.id.c_str(), tag.c_str(), POSTER_W, POSTER_H);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    bool cached=false;
    {
        uiDiagnostics().setWorker("artwork","movie ImageCache lookup");
        UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache lookup/filesystem stat",false);
        cached=ImageCache::isCached(m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H);
    }
    if(cached){
        uiDiagnostics().setWorker("artwork","movie ImageCache read");
        {
            UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache filesystem read",false);
            jpegData=ImageCache::readCached(m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H);
        }
        printf("[MovieDetailsScreen] Artwork: cache hit (%zu bytes)\n",
               jpegData.size());
    }

    // 2. If not cached, synchronous HTTP request
    if (jpegData.empty()) {
        if(m_prepareCancelled.load(std::memory_order_acquire))return {};
        HttpClient client;
        client.setTimeoutSec(8);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        bool fetched=false;
        {
            // This curl transfer was the synchronous operation in enter()
            // responsible for the hardware-proven multi-second push stall.
            uiDiagnostics().setWorker("artwork","movie Jellyfin artwork HTTP");
            UiDiagnostics::Scope scope("MovieDetailsScreen::Jellyfin artwork HTTP/curl",false);
            fetched=RouteRequest(m_session).run([&](const std::string &base){return client.getBinary(buildImageUrl(base,m_movie.id,ImageType::Primary,tag,POSTER_W,POSTER_H),headers,response,error,512*1024,&m_prepareCancelled)&&response.ok();},error);
        }
        if(!fetched){
            printf("[MovieDetailsScreen] Artwork fetch failed: %s\n",
                   error.c_str());
            return {};
        }
        if (!response.ok()) {
            printf("[MovieDetailsScreen] Artwork fetch failed: HTTP %ld\n",
                   response.status);
            return {};
        }

        jpegData = std::move(response.data);
        printf("[MovieDetailsScreen] Artwork: downloaded %zu bytes\n",
               jpegData.size());

        // Cache to disk (best-effort)
        if(!m_prepareCancelled.load(std::memory_order_acquire)){
            uiDiagnostics().setWorker("artwork","movie ImageCache write");
            UiDiagnostics::Scope scope("MovieDetailsScreen::ImageCache filesystem write",false);
            ImageCache::writeToCache(m_movie.id,ImageType::Primary,tag,
                                     POSTER_W,POSTER_H,jpegData.data(),jpegData.size());
        }
    }

    // 3. Decode JPEG
    if(m_prepareCancelled.load(std::memory_order_acquire))return {};
    uiDiagnostics().setWorker("artwork","movie JPEG decode");
    DecodedImage artwork;
    {
        UiDiagnostics::Scope scope("MovieDetailsScreen::JPEG/image decode",false);
        artwork=ImageDecoder::decodeJpeg(jpegData.data(),jpegData.size());
    }
    if (artwork.empty()) {
        printf("[MovieDetailsScreen] Artwork: decode failed\n");
    } else {
        printf("[MovieDetailsScreen] Artwork: decoded %dx%d\n",
               artwork.width, artwork.height);
    }
    return artwork;
}

// -------------------------------------------------------------------
// render
// -------------------------------------------------------------------
void MovieDetailsScreen::render(SDL_Surface *fb)
{
    if(m_firstRender){
        m_firstRender=false;
        UiDiagnostics::Scope scope("MovieDetailsScreen::first render");
        renderContent(fb);
        return;
    }
    renderContent(fb);
}

void MovieDetailsScreen::renderContent(SDL_Surface *fb)
{
    // 1. Draw movie poster placeholder (artR/artG/artB tint)
    BitmapFont::fillRect(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H,
        m_movie.artR, m_movie.artG, m_movie.artB, 255);

    // 2. Draw decoded artwork (aspect-fit, centered, no crop/stretch)
    SDL_Surface *artworkSurface=m_movieArtworkSurface?m_movieArtworkSurface:m_gridArtworkSurface;
    const DecodedImage *artwork=m_movieArtworkSurface?&m_movieArtwork:m_gridArtwork.get();
    if (artworkSurface&&artwork) {
        UiDiagnostics::Scope imageScope("MovieDetailsScreen::render image blit");
        int imgW = artwork->width;
        int imgH = artwork->height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)POSTER_W / (float)POSTER_H;

        int drawW, drawH;
        if (imgAspect > boxAspect) {
            drawW = POSTER_W;
            drawH = (int)(POSTER_W / imgAspect + 0.5f);
            if (drawH > POSTER_H) drawH = POSTER_H;
        } else {
            drawH = POSTER_H;
            drawW = (int)(POSTER_H * imgAspect + 0.5f);
            if (drawW > POSTER_W) drawW = POSTER_W;
        }
        int drawX = POSTER_X + (POSTER_W - drawW) / 2;
        int drawY = POSTER_Y + (POSTER_H - drawH) / 2;

        SDL_Rect srcRect = {0, 0, imgW, imgH};
        SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
        SDL_BlitScaled(artworkSurface,&srcRect,fb,&dstRect);
    }

    // 3. Poster border (drawn after artwork)
    BitmapFont::drawRect(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);

    // 4. Right panel: title, metadata, genres, overview
    int rx = RIGHT_X;
    int ry = RIGHT_TOP_Y;

    // Movie title
    BitmapFont::drawString(fb, rx, ry, m_movie.title.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    ry += BitmapFont::GLYPH_H + 4;

    // Metadata line: year | runtime | rating
    bool hasMeta = false;
    char metaBuf[128];
    metaBuf[0] = '\0';

    if (m_movie.year > 0) {
        std::snprintf(metaBuf, sizeof(metaBuf), "%d", m_movie.year);
        hasMeta = true;
    }
    int mins = ticksToMinutes(m_movie.runTimeTicks);
    if (mins > 0) {
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf + len, sizeof(metaBuf) - len,
                          "  |  %d min", mins);
        else {
            std::snprintf(metaBuf, sizeof(metaBuf), "%d min", mins);
            hasMeta = true;
        }
    }
    if (m_movie.rating > 0.0f) {
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf + len, sizeof(metaBuf) - len,
                          "  |  %.1f/10", (double)m_movie.rating);
        else {
            std::snprintf(metaBuf, sizeof(metaBuf), "%.1f/10",
                          (double)m_movie.rating);
            hasMeta = true;
        }
    }
    if (hasMeta) {
        BitmapFont::drawString(fb, rx, ry, metaBuf,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        ry += BitmapFont::GLYPH_H + 2;
    }

    // Genres
    if (!m_movie.genres.empty()) {
        std::string genreStr;
        for (size_t i = 0; i < m_movie.genres.size(); ++i) {
            if (i > 0) genreStr += ", ";
            genreStr += m_movie.genres[i];
        }
        BitmapFont::drawString(fb, rx, ry, genreStr.c_str(),
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        ry += BitmapFont::GLYPH_H + 4;
    } else if (!m_movie.genre.empty()) {
        BitmapFont::drawString(fb, rx, ry, m_movie.genre.c_str(),
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        ry += BitmapFont::GLYPH_H + 4;
    }

    // 5. Overview (word-wrapped, scrollable)
    bool overviewScrollable = false;
    if (!m_movie.overview.empty()) {
        int overviewEndY = BTN_Y - 4;
        int visibleLines = (overviewEndY - ry) / BitmapFont::GLYPH_H;
        if (visibleLines < 1) visibleLines = 1;

        int maxScroll = (int)m_overviewLines.size() - visibleLines;
        if (maxScroll < 0) maxScroll = 0;
        if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        if (m_overviewScroll < 0) m_overviewScroll = 0;

        overviewScrollable = (maxScroll > 0);

        int drawY = ry;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + visibleLines && i < (int)m_overviewLines.size();
             ++i) {
            BitmapFont::drawString(fb, rx, drawY, m_overviewLines[i].c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
            drawY += BitmapFont::GLYPH_H;
        }
    }

    // 6. Action buttons [PLAY] [DOWNLOAD]
    if (m_downloads && m_planId) {
        UiDiagnostics::Scope stateScope("MovieDetailsScreen::render playback/download state preparation");
        const auto &p=m_planSnapshot; std::string status;
        if(m_confirmDownload) status="Download ~"+formatBytes(p.plan.additionalRequiredBytes)+"? A=Confirm B=Cancel";
        else if(p.state==DownloadPlanState::Planning) status=p.plan.sizeKnown?"Download: ~"+formatBytes(p.plan.additionalRequiredBytes)+"  Preparing...":"Checking size...";
        else if(p.state==DownloadPlanState::Ready) status="Download: ~"+formatBytes(p.plan.additionalRequiredBytes)+"  Free: "+formatBytes(p.plan.usableFreeBytes);
        else if(p.state==DownloadPlanState::Error) status=p.plan.error;
        if(!status.empty()) BitmapFont::drawString(fb,rx,BTN_Y-20,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
    }
    auto drawBtn = [&](int bx, const char *label, bool focused) {
        if (focused) {
            BitmapFont::fillRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_OR, FOCUS_OG, FOCUS_OB, 255);
            BitmapFont::drawRect(fb, bx - 2, BTN_Y - 2,
                BTN_W + 4, BTN_H + 4, FOCUS_OR, FOCUS_OG, FOCUS_OB);
            BitmapFont::drawRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_IR, FOCUS_IG, FOCUS_IB);
        } else {
            BitmapFont::fillRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                Theme::BG_B * 2 / 3, 255);
            BitmapFont::drawRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        }
        int tw = (int)std::strlen(label) * BitmapFont::GLYPH_W;
        int tx = bx + (BTN_W - tw) / 2;
        int ty = BTN_Y + (BTN_H - BitmapFont::GLYPH_H) / 2;
        BitmapFont::drawString(fb, tx, ty, label,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            focused ? FOCUS_OR : (Theme::BG_R * 2 / 3),
            focused ? FOCUS_OG : (Theme::BG_G * 2 / 3),
            focused ? FOCUS_OB : (Theme::BG_B * 2 / 3));
    };

    bool playFocused = (m_actionBtn == ActionButton::Play);
    bool dlFocused   = (m_actionBtn == ActionButton::Download);
    drawBtn(BTN_PLAY_X, "PLAY", playFocused);
    drawBtn(BTN_DL_X,   "DOWNLOAD", dlFocused);

    // 7. Bottom hint bar
    renderBottomHints(fb, overviewScrollable
        ? "A=Select  B=Back  L/R=Bio"
        : "A=Select  B=Back");
}

} // namespace miyoofin
