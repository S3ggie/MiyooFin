#include "SeriesScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

static constexpr int HEADER_Y    = 8;
static constexpr int ROW_H       = 20;
static constexpr int BOTTOM_H    = 18;
static constexpr int FB_W        = 640;
static constexpr int FB_H        = 480;
static constexpr int PAD_X       = 16;
static constexpr int OVERVIEW_WRAP = 72;
static constexpr int MAX_VISIBLE_SEASONS = 15;

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

    // Ready
    switch (action) {
    case Action::Up:
        if (m_selectedSeason > 0) {
            m_selectedSeason--;
            if (m_selectedSeason < m_seasonScroll)
                m_seasonScroll = m_selectedSeason;
        }
        return true;
    case Action::Down:
        if (m_selectedSeason < (int)m_seasons.size() - 1) {
            m_selectedSeason++;
            if (m_selectedSeason >= m_seasonScroll + MAX_VISIBLE_SEASONS)
                m_seasonScroll = m_selectedSeason - MAX_VISIBLE_SEASONS + 1;
        }
        return true;
    case Action::Confirm: {
        const MediaItem &season = m_seasons[m_selectedSeason];
        printf("[SeriesScreen] Select season: %s index=%d\n",
               season.title.c_str(), season.indexNumber);
        return true;
    }
    case Action::Back:
        m_stack->pop();
        return true;
    default:
        return false;
    }
}

void SeriesScreen::update(Uint32 /*dt*/) {}

// -------------------------------------------------------------------
// Rendering
// -------------------------------------------------------------------

static void renderBottomHints(SDL_Surface *fb, bool errorState)
{
    int y = FB_H - BOTTOM_H;
    BitmapFont::fillRect(fb, 0, y, FB_W, BOTTOM_H,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3, 255);

    const char *hint = errorState ? "A=Retry  B=Back" : "A=Open  B=Back";
    BitmapFont::drawString(fb, PAD_X, y + 2, hint,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3);
}

void SeriesScreen::render(SDL_Surface *fb)
{
    if (m_loadState == LoadState::Loading) {
        const char *msg = "Loading seasons...";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        int my = FB_H / 3;
        BitmapFont::drawString(fb, mx, my, msg,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        return;
    }
    if (m_loadState == LoadState::Error) {
        const char *header = "Failed to load seasons";
        int hx = (FB_W - (int)std::strlen(header) * BitmapFont::GLYPH_W) / 2;
        int hy = FB_H / 3;
        BitmapFont::drawString(fb, hx, hy, header,
            Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        char errBuf[128];
        std::snprintf(errBuf, sizeof(errBuf), "%s", m_error.c_str());
        int maxCols = (FB_W - 2 * PAD_X) / BitmapFont::GLYPH_W;
        if ((int)std::strlen(errBuf) > maxCols) errBuf[maxCols] = '\0';
        int ex = (FB_W - (int)std::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
        int ey = hy + BitmapFont::GLYPH_H + 8;
        BitmapFont::drawString(fb, ex, ey, errBuf,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, true);
        return;
    }

    // Ready state
    int y = HEADER_Y;
    BitmapFont::drawString(fb, PAD_X, y, m_series.title.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    y += BitmapFont::GLYPH_H + 2;

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
        BitmapFont::drawString(fb, PAD_X, y, meta,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        y += BitmapFont::GLYPH_H + 2;
    }

    // Overview
    if (!m_series.overview.empty()) {
        BitmapFont::drawString(fb, PAD_X, y, m_series.overview.c_str(),
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B, OVERVIEW_WRAP);
        int textLen = (int)m_series.overview.size();
        int lines = (textLen + OVERVIEW_WRAP - 1) / OVERVIEW_WRAP;
        y += lines * BitmapFont::GLYPH_H + 4;
    }
    y += 4;

    // "Seasons" heading
    BitmapFont::drawString(fb, PAD_X, y, "Seasons",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    y += BitmapFont::GLYPH_H + 4;

    // Season list
    int listTop = y;
    int visibleCount = (int)m_seasons.size();
    if (visibleCount > MAX_VISIBLE_SEASONS)
        visibleCount = MAX_VISIBLE_SEASONS;

    for (int i = 0; i < visibleCount; ++i) {
        int idx = m_seasonScroll + i;
        if (idx >= (int)m_seasons.size()) break;
        const MediaItem &season = m_seasons[idx];
        bool selected = (idx == m_selectedSeason);
        int rowY = listTop + i * ROW_H;

        if (selected) {
            BitmapFont::fillRect(fb, PAD_X - 4, rowY,
                FB_W - 2 * (PAD_X - 4), ROW_H,
                Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 40);
            BitmapFont::drawString(fb, PAD_X, rowY + 2,
                season.title.c_str(),
                Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
        } else {
            BitmapFont::drawString(fb, PAD_X, rowY + 2,
                season.title.c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
        }
    }

    renderBottomHints(fb, false);
}

} // namespace miyoofin
