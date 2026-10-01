#include "EpisodeBrowserScreen.hpp"
#include "../ArtworkPresentation.hpp"
#include "../UiKit.hpp"
#include "EpisodeBrowserLayout.hpp"
#include "../../download/DownloadSupport.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace miyoofin {

static std::vector<std::string> wrapText(const char* text, int wrapCols)
{
    std::vector<std::string> lines;
    if (!text || !*text)
        return lines;
    std::string input(text);
    size_t pos = 0;
    while (pos < input.size()) {
        size_t newline = input.find('\n', pos);
        std::string para =
            newline != std::string::npos ? input.substr(pos, newline - pos) : input.substr(pos);
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

static std::string formatEpNum(int indexNumber)
{
    if (indexNumber <= 0)
        return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "E%02d", indexNumber);
    return std::string(buf);
}

void EpisodeBrowserScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = m_series.title +
                   (m_season.title.empty() ? std::string() : "  /  " + m_season.title);
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
        centered("Loading episodes...", "", d::kText);
        footer.hints = {{ui::Key::B, "Back"}};
        ui::footer(fb, footer);
        return;
    }
    if (m_loadState == LoadState::Error) {
        centered("Could not load episodes", m_error, d::kDanger);
        footer.hints = {{ui::Key::A, "Retry"}, {ui::Key::B, "Back"}};
        ui::footer(fb, footer);
        return;
    }

    const int total = (int)m_episodes.size();
    if (total <= 0 || m_selectedEpisode < 0 || m_selectedEpisode >= total) {
        centered("No episodes", "This season has no episodes yet.", d::kTextSecondary);
        footer.hints = {{ui::Key::B, "Back"}};
        ui::footer(fb, footer);
        return;
    }

    // ---- left: episode list
    for (int vis = 0; vis < EB_LIST_VISIBLE; ++vis) {
        const int idx = m_listScroll + vis;
        if (idx >= total)
            break;
        const MediaItem& ep = m_episodes[idx];
        const int ry = EB_LIST_Y + vis * EB_ROW_PITCH;
        const bool isSel = idx == m_selectedEpisode;
        const bool focused = isSel && m_focus == FocusArea::EpisodeList;
        if (focused)
            ui::focusRing(fb, EB_LIST_X, ry, EB_LIST_W, EB_ROW_H);
        ui::roundFill(fb, EB_LIST_X, ry, EB_LIST_W, EB_ROW_H, d::kRadius,
                      focused ? d::kRaised : (isSel ? d::kAccentSoft : d::kPanel));
        ui::roundOutline(fb, EB_LIST_X, ry, EB_LIST_W, EB_ROW_H, d::kRadius,
                         focused ? d::kAccent : (isSel ? d::kAccentDim : d::kBorder));

        const std::string num = formatEpNum(ep.indexNumber);
        int tx = EB_LIST_X + 10;
        if (!num.empty())
            tx += ui::chip(fb, tx, ry + 9, num, focused ? d::kAccent : d::kRaised,
                           focused ? d::Rgb{255, 255, 255} : d::kTextSecondary) + 8;
        const bool partial = !ep.played && playbackPercent(ep) > 0;
        const int rightPad = ep.played ? 26 : 10;
        ui::textClamped(fb, tx, ry + 11, EB_LIST_X + EB_LIST_W - rightPad - tx, ep.title,
                        isSel ? d::kText : d::kTextSecondary);
        if (ep.played)
            ui::iconCheck(fb, EB_LIST_X + EB_LIST_W - 22, ry + 13, 12, d::kSuccess);
        else if (partial)
            ui::progressBar(fb, EB_LIST_X + 8, ry + EB_ROW_H - 6, EB_LIST_W - 16, 3,
                            playbackPercent(ep), d::kAccent);
    }
    if (total > EB_LIST_VISIBLE) {
        const int trackH = EB_LIST_VISIBLE * EB_ROW_PITCH - 4;
        const int thumbH = std::max(16, trackH * EB_LIST_VISIBLE / total);
        const int thumbY = EB_LIST_Y + (trackH - thumbH) * m_listScroll /
                                           std::max(1, total - EB_LIST_VISIBLE);
        ui::fill(fb, EB_LIST_X + EB_LIST_W + 4, EB_LIST_Y, 3, trackH, d::kDivider);
        ui::roundFill(fb, EB_LIST_X + EB_LIST_W + 4, thumbY, 3, thumbH, 1, d::kAccentDim);
    }

    // ---- right: thumbnail, facts, bio, actions
    const MediaItem& ep = m_episodes[m_selectedEpisode];
    constexpr int radius = 6;
    if (m_episodeArtworkSurface.surface) {
        SDL_Rect dst = {EB_THUMB_X, EB_THUMB_Y, m_episodeArtworkSurface.surface->w,
                        m_episodeArtworkSurface.surface->h};
        SDL_BlitSurface(m_episodeArtworkSurface.surface, nullptr, fb, &dst);
    } else {
        ui::placeholderTile(fb, EB_THUMB_X, EB_THUMB_Y, EB_THUMB_W, EB_THUMB_H, ep.title, radius);
    }
    ui::roundCorners(fb, EB_THUMB_X, EB_THUMB_Y, EB_THUMB_W, EB_THUMB_H, radius, d::kCanvas);
    if (!ep.played && playbackPercent(ep) > 0) {
        ui::bottomScrim(fb, EB_THUMB_X, EB_THUMB_Y, EB_THUMB_W, EB_THUMB_H, 26, 200);
        ui::progressBar(fb, EB_THUMB_X + 10, EB_THUMB_Y + EB_THUMB_H - 14, EB_THUMB_W - 20, 4,
                        playbackPercent(ep), d::kAccent);
    }
    ui::roundOutline(fb, EB_THUMB_X, EB_THUMB_Y, EB_THUMB_W, EB_THUMB_H, radius, d::kBorder);

    ui::textClamped(fb, EB_RIGHT_X + 10, EB_TITLE_Y, EB_RIGHT_W - 10, ep.title, d::kText);
    int cx = EB_RIGHT_X + 10;
    auto addChip = [&](const std::string& label, d::Rgb bg, d::Rgb fg) {
        if (cx + ui::textWidth(label) + 12 > d::kScreenW - d::kMargin)
            return;
        cx += ui::chip(fb, cx, EB_CHIPS_Y, label, bg, fg) + 6;
    };
    if (ep.parentIndexNumber > 0 && ep.indexNumber > 0)
        addChip("S" + std::to_string(ep.parentIndexNumber) + " E" + std::to_string(ep.indexNumber),
                d::kAccentSoft, d::kAccentHi);
    if (ep.runTimeTicks > 0)
        addChip(std::to_string(ticksToMinutes(ep.runTimeTicks)) + " min", d::kRaised,
                d::kTextSecondary);
    if (ep.rating > 0.0f) {
        char rating[16];
        std::snprintf(rating, sizeof(rating), "%.1f", (double)ep.rating);
        addChip(rating, d::kRaised, d::kTextSecondary);
    }
    if (ep.played)
        addChip("Watched", ui::mix(d::kCanvas, d::kSuccess, 22), d::kSuccess);

    bool overviewScrollable = false;
    if (!ep.overview.empty()) {
        const auto lines = wrapText(ep.overview.c_str(), EB_META_WRAP);
        const int vis = episodeOverviewVisibleLines();
        const int maxScroll = std::max(0, (int)lines.size() - vis);
        m_overviewScroll = std::max(0, std::min(m_overviewScroll, maxScroll));
        overviewScrollable = maxScroll > 0;
        int y = EB_OVERVIEW_Y;
        for (int i = m_overviewScroll; i < m_overviewScroll + vis && i < (int)lines.size();
             ++i, y += EB_OVERVIEW_PITCH)
            ui::text(fb, EB_RIGHT_X + 10, y, lines[i], d::kTextSecondary);
        if (overviewScrollable) {
            const int trackH = vis * EB_OVERVIEW_PITCH - 2;
            const int thumbH = std::max(14, trackH * vis / (int)lines.size());
            const int thumbY = EB_OVERVIEW_Y + (trackH - thumbH) * m_overviewScroll / maxScroll;
            ui::fill(fb, d::kScreenW - 10, EB_OVERVIEW_Y, 3, trackH, d::kDivider);
            ui::roundFill(fb, d::kScreenW - 10, thumbY, 3, thumbH, 1, d::kAccentDim);
        }
    }

    // Download plan status above the buttons.
    if (m_downloads && m_planId) {
        auto p = m_downloads->planSnapshot(m_planId);
        std::string status;
        d::Rgb color = d::kTextSecondary;
        if (m_confirmDownload) {
            footer.message = std::string("Download ") +
                             (m_planIsSeason ? "season " + std::to_string(m_season.indexNumber)
                                             : "episode") +
                             "?  A confirm  B cancel";
            footer.messageColor = d::kAccentHi;
        } else if (p.state == DownloadPlanState::Planning) {
            status = p.plan.sizeKnown ? "~" + formatBytes(p.plan.additionalRequiredBytes) +
                                            "  preparing..."
                                      : std::string("Preparing download...");
        } else if (p.state == DownloadPlanState::Ready) {
            status = std::to_string(p.itemCount) + " eps  ~" +
                     formatBytes(p.plan.additionalRequiredBytes) + "  free " +
                     formatBytes(p.plan.usableFreeBytes);
        } else if (p.state == DownloadPlanState::Error) {
            status = p.plan.error;
            color = d::kDanger;
        }
        if (!status.empty())
            ui::textClamped(fb, EB_RIGHT_X + 10, EB_BTN_Y - 24, EB_RIGHT_W - 10, status, color);
    }

    const bool onButtons = m_focus == FocusArea::ActionButtons;
    ui::button(fb, 316, EB_BTN_Y, 72, EB_BTN_H, "Play", ui::ButtonStyle::Primary,
               onButtons && m_actionBtn == ActionButton::Play);
    ui::button(fb, 398, EB_BTN_Y, 112, EB_BTN_H, "Get Episode", ui::ButtonStyle::Secondary,
               onButtons && m_actionBtn == ActionButton::DownloadEpisode);
    ui::button(fb, 520, EB_BTN_Y, 104, EB_BTN_H, "Get Season", ui::ButtonStyle::Secondary,
               onButtons && m_actionBtn == ActionButton::DownloadSeason);

    footer.hints = {{ui::Key::Dpad, "Move"}, {ui::Key::A, "Select"}, {ui::Key::B, "Back"},
                    {ui::Key::Y, "Get season"}};
    if (overviewScrollable)
        footer.hints.push_back({ui::Key::LR, "Bio"});
    ui::footer(fb, footer);
}

} // namespace miyoofin
