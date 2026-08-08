#include "SeriesScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
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
static constexpr int POSTER_W       = 79;
static constexpr int POSTER_H       = 111;
static constexpr int OVERLAY_H      = 18;    // title bar at bottom of poster

// Right-side show poster placeholder
static constexpr int SHOW_X         = 360;
static constexpr int SHOW_Y         = 51;
static constexpr int SHOW_W         = 197;
static constexpr int SHOW_H         = 239;

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

SeriesScreen::SeriesScreen(const Session &session, const MediaItem &series)
    : m_session(session)
    , m_series(series)
{
}

void SeriesScreen::enter()
{
    printf("[SeriesScreen] enter series=%s\n", m_series.title.c_str());
    if (m_seasons.empty())
        fetchSeasons();
}

void SeriesScreen::leave()
{
    printf("[SeriesScreen] leave series=%s\n", m_series.title.c_str());
}

void SeriesScreen::fetchSeasons()
{
    m_loadState = LoadState::Loading;
    m_error.clear();

    std::string error;
    bool ok = JellyfinApi::getSeasons(
        m_session.serverUrl,
        m_session.accessToken,
        m_session.userId,
        m_session.deviceId,
        m_series.id,
        m_seasons,
        error);

    if (ok) {
        m_loadState = LoadState::Ready;
        m_selectedSeason = 0;
        m_seasonScroll = 0;
        m_gridScroll = 0;
        m_overviewScroll = 0;
        printf("[SeriesScreen] Loaded %d seasons\n", (int)m_seasons.size());
    } else {
        m_loadState = LoadState::Error;
        m_error = error.empty() ? "Unknown error" : error;
        printf("[SeriesScreen] Failed to load seasons: %s\n", m_error.c_str());
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

void SeriesScreen::update(Uint32 /*dt*/) {}

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

/// Draw a season poster card (placeholder colour + title overlay + border).
static void drawSeasonPoster(SDL_Surface *fb, int x, int y, int w, int h,
                              const MediaItem &season, bool selected)
{
    // Placeholder colour fill
    BitmapFont::fillRect(fb, x, y, w, h,
        season.artR, season.artG, season.artB, 255);

    // Dark overlay along the bottom for the title
    int overlayY = y + h - OVERLAY_H;
    BitmapFont::fillRect(fb, x, overlayY, w, OVERLAY_H, 0, 0, 0, 160);

    // Season title (truncated to fit)
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s", season.title.c_str());
    int maxChars = (w - 4) / BitmapFont::GLYPH_W;
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
        drawSeasonPoster(fb, px, py, POSTER_W, POSTER_H,
                         m_seasons[itemIdx], sel);
    }

    // 3. Right-side show poster placeholder
    BitmapFont::fillRect(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H,
        m_series.artR, m_series.artG, m_series.artB, 255);
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
        ? "A=Open  B=Back  L/R=Bio" : "A=Open  B=Back");
}

} // namespace miyoofin
