#include "MovieDetailsScreen.hpp"
#include "MovieDetailsScreenInternal.hpp"
#include "../ArtworkPresentation.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../UiKit.hpp"
#include "../BitmapFont.hpp"
#include "../../image/ImageDecoder.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

void MovieDetailsScreen::render(SDL_Surface* fb)
{
    if (m_firstRender) {
        m_firstRender = false;
        UiDiagnostics::Scope scope("MovieDetailsScreen::first render");
        renderContent(fb);
        return;
    }
    renderContent(fb);
}

void MovieDetailsScreen::renderContent(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = "Movie";
    header.showStatus = false;
    ui::header(fb, header);

    // Poster: covers the card (or fills ambiently), rounded, with a hairline.
    const DecodedImage* artwork = m_movieArtworkSurface ? &m_movieArtwork : m_gridArtwork.get();
    if (artwork && !artwork->empty()) {
        if (m_posterCardSource != artwork) {
            if (m_posterCard)
                SDL_FreeSurface(m_posterCard);
            m_posterCard = ui::artworkSurface(*artwork, POSTER_W, POSTER_H);
            m_posterCardSource = artwork;
        }
    }
    if (m_posterCard) {
        SDL_Rect dst = {POSTER_X, POSTER_Y, POSTER_W, POSTER_H};
        SDL_BlitSurface(m_posterCard, nullptr, fb, &dst);
    } else {
        ui::placeholderTile(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H, m_movie.title, 6);
    }
    ui::roundCorners(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H, 6, d::kCanvas);
    ui::roundOutline(fb, POSTER_X, POSTER_Y, POSTER_W, POSTER_H, 6, d::kBorder);

    const int rx = RIGHT_X;
    const int rw = d::kScreenW - d::kMargin - RIGHT_X;
    int ry = RIGHT_TOP_Y;

    // Title: large when it fits, otherwise regular size.
    const bool big = ui::textWidth(m_movie.title, 2) <= rw;
    ui::textClamped(fb, rx, ry, rw, m_movie.title, d::kText, big ? 2 : 1);
    ry += big ? 38 : 22;

    // Facts as chips.
    int cx = rx;
    auto addChip = [&](const std::string& label, d::Rgb bg, d::Rgb fg) {
        if (cx + ui::textWidth(label) + 12 > rx + rw)
            return;
        cx += ui::chip(fb, cx, ry, label, bg, fg) + 6;
    };
    if (m_movie.year > 0)
        addChip(std::to_string(m_movie.year), d::kRaised, d::kTextSecondary);
    const int mins = ticksToMinutes(m_movie.runTimeTicks);
    if (mins > 0) {
        char runtime[24];
        std::snprintf(runtime, sizeof(runtime), "%dh %dm", mins / 60, mins % 60);
        addChip(runtime, d::kRaised, d::kTextSecondary);
    }
    if (m_movie.rating > 0.0f) {
        char rating[16];
        std::snprintf(rating, sizeof(rating), "%.1f", static_cast<double>(m_movie.rating));
        addChip(rating, d::kAccentSoft, d::kAccentHi);
    }
    if (!m_movie.genres.empty()) {
        for (std::size_t i = 0; i < m_movie.genres.size() && i < 3; ++i)
            addChip(m_movie.genres[i], d::kRaised, d::kTextSecondary);
    } else if (!m_movie.genre.empty()) {
        addChip(m_movie.genre, d::kRaised, d::kTextSecondary);
    }
    ry += 28;

    // Overview (word-wrapped, scrollable).
    constexpr int kLinePitch = 18;
    bool overviewScrollable = false;
    const int overviewEndY = BTN_Y - 52;
    if (!m_movie.overview.empty()) {
        const int visibleLines = std::max(1, (overviewEndY - ry) / kLinePitch);
        const int maxScroll = std::max(0, static_cast<int>(m_overviewLines.size()) - visibleLines);
        m_overviewScroll = std::max(0, std::min(m_overviewScroll, maxScroll));
        overviewScrollable = maxScroll > 0;
        int y = ry;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + visibleLines && i < static_cast<int>(m_overviewLines.size());
             ++i, y += kLinePitch)
            ui::text(fb, rx, y, m_overviewLines[i], d::kTextSecondary);
        if (overviewScrollable) {
            const int trackH = visibleLines * kLinePitch - 2;
            const int thumbH =
                std::max(14, trackH * visibleLines / static_cast<int>(m_overviewLines.size()));
            const int thumbY = ry + (trackH - thumbH) * m_overviewScroll / maxScroll;
            ui::fill(fb, d::kScreenW - 10, ry, 3, trackH, d::kDivider);
            ui::roundFill(fb, d::kScreenW - 10, thumbY, 3, thumbH, 1, d::kAccentDim);
        }
    } else {
        ui::text(fb, rx, ry, "No overview available.", d::kTextMuted);
    }

    // Download plan status, then the action buttons.
    ui::FooterSpec footer;
    footer.showLink = false;
    std::string status;
    d::Rgb statusColor = d::kTextSecondary;
    if (m_downloads && m_planId) {
        const auto& p = m_planSnapshot;
        if (m_confirmDownload) {
            footer.message = "Download ~" + formatBytes(p.plan.additionalRequiredBytes) +
                             "?  A confirm  B cancel";
            footer.messageColor = d::kAccentHi;
        } else if (p.state == DownloadPlanState::Planning) {
            status = p.plan.sizeKnown ? "Download ~" + formatBytes(p.plan.additionalRequiredBytes) +
                                            "  preparing..."
                                      : "Checking download size...";
        } else if (p.state == DownloadPlanState::Ready) {
            status = "Download ~" + formatBytes(p.plan.additionalRequiredBytes) + "   Free " +
                     formatBytes(p.plan.usableFreeBytes);
        } else if (p.state == DownloadPlanState::Error) {
            status = p.plan.error;
            statusColor = d::kDanger;
        }
    }
    if (!status.empty())
        ui::textClamped(fb, rx, BTN_Y - 26, rw, status, statusColor);

    ui::button(fb, BTN_PLAY_X, BTN_Y, BTN_W, BTN_H, "Play", ui::ButtonStyle::Primary,
               m_actionBtn == ActionButton::Play);
    ui::button(fb, BTN_DL_X, BTN_Y, BTN_W, BTN_H, "Download", ui::ButtonStyle::Secondary,
               m_actionBtn == ActionButton::Download);

    footer.hints = {{ui::Key::Dpad, "Move"}, {ui::Key::A, "Select"}, {ui::Key::B, "Back"}};
    if (overviewScrollable)
        footer.hints.push_back({ui::Key::LR, "Scroll bio"});
    ui::footer(fb, footer);
    m_audioMenu.render(fb);
}
}
