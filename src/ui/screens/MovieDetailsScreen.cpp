#include "MovieDetailsScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../cache/ImageCache.hpp"
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
                                       const MediaItem &movie)
    : m_session(session)
    , m_movie(movie)
{
}

// -------------------------------------------------------------------
// enter — attempt artwork load
// -------------------------------------------------------------------
void MovieDetailsScreen::enter()
{
    printf("[MovieDetailsScreen] enter: %s\n", m_movie.title.c_str());
    tryLoadMovieArtwork();
}

// -------------------------------------------------------------------
// leave
// -------------------------------------------------------------------
void MovieDetailsScreen::leave()
{
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
        if (m_actionBtn == ActionButton::Play) {
            printf("[MovieDetailsScreen] Play selected: %s\n",
                   m_movie.title.c_str());
            std::string error;
            if (PlaybackRequest::write(m_movie.id, "movie",
                                       m_movie.playbackPositionTicks, error)) {
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
            printf("[MovieDetailsScreen] Download selected: %s\n",
                   m_movie.title.c_str());
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
// tryLoadMovieArtwork — synchronous artwork fetch/cache/decode
// -------------------------------------------------------------------
void MovieDetailsScreen::tryLoadMovieArtwork()
{
    if (m_movieArtworkAttempted) return;
    m_movieArtworkAttempted = true;

    auto it = m_movie.imageTags.find("Primary");
    if (it == m_movie.imageTags.end() || it->second.empty()) {
        printf("[MovieDetailsScreen] No Primary artwork tag for %s\n",
               m_movie.title.c_str());
        return;
    }

    const std::string &tag = it->second;
    printf("[MovieDetailsScreen] Artwork: loading %s tag=%s (%dx%d)\n",
           m_movie.id.c_str(), tag.c_str(), POSTER_W, POSTER_H);

    // 1. Check disk cache
    std::vector<unsigned char> jpegData;
    if (ImageCache::isCached(m_movie.id, ImageType::Primary, tag,
                             POSTER_W, POSTER_H)) {
        jpegData = ImageCache::readCached(m_movie.id, ImageType::Primary,
                                          tag, POSTER_W, POSTER_H);
        printf("[MovieDetailsScreen] Artwork: cache hit (%zu bytes)\n",
               jpegData.size());
    }

    // 2. If not cached, synchronous HTTP request
    if (jpegData.empty()) {
        std::string url = buildImageUrl(
            m_session.serverUrl, m_movie.id, ImageType::Primary,
            tag, POSTER_W, POSTER_H);

        HttpClient client;
        client.setTimeoutSec(8);
        auto headers = JellyfinApi::buildAuthHeaders(
            m_session.accessToken, m_session.deviceId);

        BinaryHttpResponse response;
        std::string error;
        if (!client.getBinary(url, headers, response, error, 512 * 1024)) {
            printf("[MovieDetailsScreen] Artwork fetch failed: %s\n",
                   error.c_str());
            return;
        }
        if (!response.ok()) {
            printf("[MovieDetailsScreen] Artwork fetch failed: HTTP %ld\n",
                   response.status);
            return;
        }

        jpegData = std::move(response.data);
        printf("[MovieDetailsScreen] Artwork: downloaded %zu bytes\n",
               jpegData.size());

        // Cache to disk (best-effort)
        ImageCache::writeToCache(m_movie.id, ImageType::Primary, tag,
                                 POSTER_W, POSTER_H,
                                 jpegData.data(), jpegData.size());
    }

    // 3. Decode JPEG
    m_movieArtwork = ImageDecoder::decodeJpeg(jpegData.data(),
                                              jpegData.size());
    if (m_movieArtwork.empty()) {
        printf("[MovieDetailsScreen] Artwork: decode failed\n");
    } else {
        printf("[MovieDetailsScreen] Artwork: decoded %dx%d\n",
               m_movieArtwork.width, m_movieArtwork.height);
    }
}

// -------------------------------------------------------------------
// render
// -------------------------------------------------------------------
void MovieDetailsScreen::render(SDL_Surface *fb)
{
    // 1. Draw movie poster placeholder (artR/artG/artB tint)
    BitmapFont::fillRect(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H,
        m_movie.artR, m_movie.artG, m_movie.artB, 255);

    // 2. Draw decoded artwork (aspect-fit, centered, no crop/stretch)
    if (!m_movieArtwork.empty()) {
        int imgW = m_movieArtwork.width;
        int imgH = m_movieArtwork.height;
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

        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_movieArtwork.pixels.data(),
            imgW, imgH, 32, imgW * 4,
            0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
        if (imgSurface) {
            SDL_Rect srcRect = {0, 0, imgW, imgH};
            SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
            SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
            SDL_FreeSurface(imgSurface);
        }
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
        auto lines = wrapText(m_movie.overview.c_str(), META_WRAP);
        int overviewEndY = BTN_Y - 4;
        int visibleLines = (overviewEndY - ry) / BitmapFont::GLYPH_H;
        if (visibleLines < 1) visibleLines = 1;

        int maxScroll = (int)lines.size() - visibleLines;
        if (maxScroll < 0) maxScroll = 0;
        if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        if (m_overviewScroll < 0) m_overviewScroll = 0;

        overviewScrollable = (maxScroll > 0);

        int drawY = ry;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + visibleLines && i < (int)lines.size();
             ++i) {
            BitmapFont::drawString(fb, rx, drawY, lines[i].c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
            drawY += BitmapFont::GLYPH_H;
        }
    }

    // 6. Action buttons [PLAY] [DOWNLOAD]
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
