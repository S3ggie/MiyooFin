#include "SeriesScreen.hpp"
#include "SeriesScreenInternal.hpp"
#include "../ArtworkPresentation.hpp"
#include "../UiKit.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

void SeriesScreen::clampGridScroll()
{
    if (m_seasons.empty()) {
        m_gridScroll = 0;
        return;
    }
    int selRow = m_selectedSeason / GRID_COLS;
    int totalRows = gridRowCount((int)m_seasons.size());

    if (selRow < m_gridScroll)
        m_gridScroll = selRow;
    else if (selRow >= m_gridScroll + GRID_ROWS)
        m_gridScroll = selRow - GRID_ROWS + 1;

    if (m_gridScroll < 0)
        m_gridScroll = 0;
    int maxScroll = totalRows - GRID_ROWS;
    if (maxScroll < 0)
        maxScroll = 0;
    if (m_gridScroll > maxScroll)
        m_gridScroll = maxScroll;
}

// -------------------------------------------------------------------
// Rendering helpers
// -------------------------------------------------------------------

SeriesScreen::PreparedArtwork SeriesScreen::prepareArtworkSurface(const DecodedImage& image,
                                                                  int boxX, int boxY, int boxW,
                                                                  int boxH)
{
    PreparedArtwork prepared;
    if (image.empty())
        return prepared;
    // Exactly boxW x boxH: covered, or ambient-filled; never letterboxed.
    prepared.surface = ui::artworkSurface(image, boxW, boxH);
    prepared.x = boxX;
    prepared.y = boxY;
    return prepared;
}

void SeriesScreen::freePreparedArtwork(PreparedArtwork& artwork)
{
    if (artwork.surface)
        SDL_FreeSurface(artwork.surface);
    artwork = {};
}

/// Draw a season card: artwork (or a styled placeholder), rounded, with a
/// caption underneath. The selected card gets the focus ring.
void SeriesScreen::drawSeasonPoster(SDL_Surface* fb, int x, int y, int w, int h,
                                    const MediaItem& season, bool selected,
                                    const PreparedArtwork* artwork)
{
    namespace d = design;
    constexpr int radius = 5;
    if (artwork && artwork->surface) {
        SDL_Rect dstRect = {x, y, artwork->surface->w, artwork->surface->h};
        SDL_BlitSurface(artwork->surface, nullptr, fb, &dstRect);
    } else {
        ui::placeholderTile(fb, x, y, w, h, season.title, radius);
    }
    ui::roundCorners(fb, x, y, w, h, radius, d::kCanvas);
    if (selected)
        ui::focusRing(fb, x, y, w, h, radius);
    else
        ui::roundOutline(fb, x, y, w, h, radius, d::kBorder);
    ui::textClamped(fb, x, y + h + 6, w, season.title, selected ? d::kText : d::kTextSecondary);
}

// -------------------------------------------------------------------
// Main render
// -------------------------------------------------------------------

void SeriesScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = "Show";
    header.showStatus = false;
    ui::header(fb, header);

    ui::FooterSpec footer;
    footer.showLink = false;

    auto centered = [&](const std::string& title, const std::string& detail, d::Rgb color) {
        const int w = 360, h = detail.empty() ? 64 : 92;
        const int x = (d::kScreenW - w) / 2, y = 170;
        ui::panel(fb, x, y, w, h);
        ui::text(fb, x + (w - ui::textWidth(title)) / 2, y + 18, title, color);
        int ly = y + 44;
        for (const std::string& line : ui::wrap(detail, w - 32, 2)) {
            ui::text(fb, x + (w - ui::textWidth(line)) / 2, ly, line, d::kTextSecondary);
            ly += 18;
        }
    };

    if (m_loadState == LoadState::Loading) {
        centered("Loading seasons...", "", d::kText);
        footer.hints = {{ui::Key::B, "Back"}};
        ui::footer(fb, footer);
        return;
    }
    if (m_loadState == LoadState::Error) {
        centered("Could not load seasons", m_error, d::kDanger);
        footer.hints = {{ui::Key::A, "Retry"}, {ui::Key::B, "Back"}};
        ui::footer(fb, footer);
        return;
    }

    // ---- left column: show poster, facts, bio
    {
        constexpr int radius = 6;
        if (m_seriesArtworkSurface.surface) {
            SDL_Rect dst = {SHOW_X, SHOW_Y, m_seriesArtworkSurface.surface->w,
                            m_seriesArtworkSurface.surface->h};
            SDL_BlitSurface(m_seriesArtworkSurface.surface, nullptr, fb, &dst);
        } else {
            ui::placeholderTile(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H, m_series.title, radius);
        }
        ui::roundCorners(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H, radius, d::kCanvas);
        ui::roundOutline(fb, SHOW_X, SHOW_Y, SHOW_W, SHOW_H, radius, d::kBorder);

        const int tx = SHOW_X + SHOW_W + 12;
        const int tw = 300 - 12 - tx;
        int ty = SHOW_Y;
        for (const std::string& line : ui::wrap(m_series.title, tw, 3)) {
            ui::text(fb, tx, ty, line, d::kText);
            ty += 18;
        }
        ty += 6;
        auto chipLine = [&](const std::string& label, d::Rgb bg, d::Rgb fg) {
            ui::chip(fb, tx, ty, ui::fit(label, tw - 12), bg, fg);
            ty += 26;
        };
        if (m_series.year > 0)
            chipLine(std::to_string(m_series.year), d::kRaised, d::kTextSecondary);
        if (!m_series.genre.empty())
            chipLine(m_series.genre, d::kRaised, d::kTextSecondary);
        if (!m_seasons.empty())
            chipLine(std::to_string(m_seasons.size()) +
                         (m_seasons.size() == 1 ? " season" : " seasons"),
                     d::kAccentSoft, d::kAccentHi);
    }

    bool overviewScrollable = false;
    if (!m_series.overview.empty()) {
        const auto lines = wrapOverview(m_series.overview.c_str(), META_WRAP);
        const int visible = overviewVisibleLines();
        const int maxScroll = std::max(0, static_cast<int>(lines.size()) - visible);
        m_overviewScroll = std::max(0, std::min(m_overviewScroll, maxScroll));
        overviewScrollable = maxScroll > 0;
        int y = OVERVIEW_Y;
        for (int i = m_overviewScroll; i < m_overviewScroll + visible && i < (int)lines.size();
             ++i, y += OVERVIEW_PITCH)
            ui::text(fb, META_X, y, lines[i], d::kTextSecondary);
        if (overviewScrollable) {
            const int trackH = visible * OVERVIEW_PITCH - 2;
            const int thumbH = std::max(14, trackH * visible / static_cast<int>(lines.size()));
            const int thumbY = OVERVIEW_Y + (trackH - thumbH) * m_overviewScroll / maxScroll;
            ui::fill(fb, 284, OVERVIEW_Y, 3, trackH, d::kDivider);
            ui::roundFill(fb, 284, thumbY, 3, thumbH, 1, d::kAccentDim);
        }
    }
    ui::fill(fb, 292, d::kHeaderH + 12, 1, d::kScreenH - d::kFooterH - d::kHeaderH - 24,
             d::kDivider);

    // ---- right column: heading with download plan status, then season cards
    const int gridX = COL_X[0];
    ui::fill(fb, gridX, GRID_HEAD_Y + 2, 3, 12, d::kAccent);
    ui::text(fb, gridX + 10, GRID_HEAD_Y, "Seasons", d::kText);
    if (m_downloads && m_planId) {
        auto p = m_downloads->planSnapshot(m_planId);
        std::string status;
        d::Rgb color = d::kTextSecondary;
        const char* what = m_planWholeSeries ? "series" : "season";
        if (m_confirmDownload) {
            footer.message = std::string("Download ") + what + "?  A confirm  B cancel";
            footer.messageColor = d::kAccentHi;
        } else if (p.state == DownloadPlanState::Planning) {
            status = p.plan.sizeKnown ? std::to_string(p.itemCount) + " eps  ~" +
                                            formatBytes(p.plan.additionalRequiredBytes)
                                      : std::string("Planning ") + what + "...";
        } else if (p.state == DownloadPlanState::Ready) {
            status = std::to_string(p.itemCount) + " eps  ~" +
                     formatBytes(p.plan.additionalRequiredBytes) + "  free " +
                     formatBytes(p.plan.usableFreeBytes);
        } else if (p.state == DownloadPlanState::Error) {
            status = p.plan.error;
            color = d::kDanger;
        }
        if (!status.empty()) {
            const int avail = d::kScreenW - d::kMargin - (gridX + 10 + 7 * 8 + 12);
            const std::string shown = ui::fit(status, avail);
            ui::text(fb, d::kScreenW - d::kMargin - ui::textWidth(shown), GRID_HEAD_Y, shown,
                     color);
        }
    }

    const int totalSeasons = (int)m_seasons.size();
    for (int vis = 0; vis < GRID_VISIBLE; ++vis) {
        const int gridRow = vis / GRID_COLS, gridCol = vis % GRID_COLS;
        const int itemIdx = (m_gridScroll + gridRow) * GRID_COLS + gridCol;
        if (itemIdx >= totalSeasons)
            break;
        const PreparedArtwork* artPtr = nullptr;
        const std::string key = seasonArtworkKey(m_seasons[itemIdx]);
        if (!key.empty()) {
            auto it = m_seasonArtworkSurfaces.find(key);
            if (it != m_seasonArtworkSurfaces.end() && it->second.surface)
                artPtr = &it->second;
        }
        drawSeasonPoster(fb, COL_X[gridCol], GRID_TOP_Y + gridRow * GRID_ROW_H, POSTER_W, POSTER_H,
                         m_seasons[itemIdx], itemIdx == m_selectedSeason, artPtr);
    }
    if (totalSeasons > GRID_VISIBLE) {
        const int rows = gridRowCount(totalSeasons);
        const int trackH = GRID_ROWS * GRID_ROW_H - 16;
        const int thumbH = std::max(16, trackH * GRID_ROWS / rows);
        const int thumbY =
            GRID_TOP_Y + (trackH - thumbH) * m_gridScroll / std::max(1, rows - GRID_ROWS);
        ui::fill(fb, d::kScreenW - 8, GRID_TOP_Y, 3, trackH, d::kDivider);
        ui::roundFill(fb, d::kScreenW - 8, thumbY, 3, thumbH, 1, d::kAccentDim);
    }

    footer.hints = {{ui::Key::Dpad, "Move"},
                    {ui::Key::A, "Open"},
                    {ui::Key::B, "Back"},
                    {ui::Key::Y, "Season"},
                    {ui::Key::X, "Series"}};
    if (overviewScrollable)
        footer.hints.push_back({ui::Key::LR, "Bio"});
    ui::footer(fb, footer);
}

}
