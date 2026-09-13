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
#include "../../diagnostics/PerformanceTelemetry.hpp"
#include "../../diagnostics/TelemetryGuards.hpp"
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
                m_stack->requestExternalPlayback(
                    source == PlaybackSource::Local
                        ? ScreenStack::ExternalPlaybackSource::Local
                        : ScreenStack::ExternalPlaybackSource::Jellyfin);
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

} // namespace miyoofin
