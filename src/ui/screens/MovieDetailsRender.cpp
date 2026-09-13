#include "MovieDetailsScreen.hpp"
#include "MovieDetailsScreenInternal.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../image/ImageDecoder.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/JellyfinApi.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

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
}
