#include "SeriesScreen.hpp"
#include "SeriesScreenInternal.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

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

}
