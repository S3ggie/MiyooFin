#include "../../music/MusicTypes.hpp"
#include "../Design.hpp"
#include "../UiKit.hpp"
#include "MusicScreen.hpp"
#include <algorithm>
#include <cstdio>

namespace miyoofin {

namespace {

namespace d = design;

bool isAlphabetical(MusicPaneKind k)
{
    return k == MusicPaneKind::Artists || k == MusicPaneKind::Albums || k == MusicPaneKind::Songs;
}

constexpr int kSubBarTop = 44, kSubBarH = 28;
constexpr int kRowHeight = 44;
constexpr int kMiniTop = 404, kMiniH = 52;

// Horizontal purple -> magenta gradient (focus markers, progress bars).
void gradientBar(SDL_Surface* fb, int x, int y, int w, int h, int fillW)
{
    fillW = std::max(0, std::min(fillW, w));
    for (int i = 0; i < fillW; ++i) {
        const d::Rgb c = ui::mix(d::kAccent, d::kAccentHi, w > 1 ? i * 100 / (w - 1) : 0);
        ui::fill(fb, x + i, y, 1, h, c);
    }
}

std::string clock(double seconds)
{
    return music::formatDuration(static_cast<int>(seconds < 0 ? 0 : seconds));
}

} // namespace

// ---- covers
// ---------------------------------------------------------------------------------------

void MusicScreen::drawCover(SDL_Surface* fb, const std::string& id, const std::string& tag, int x,
                            int y, int size, int requestSize, const std::string& label)
{
    SDL_Surface* s = cover(id, tag, requestSize, true);
    if (s) {
        SDL_Rect dst = {x, y, size, size};
        SDL_BlitScaled(s, nullptr, fb, &dst);
        ui::roundCorners(fb, x, y, size, size, std::min(4, size / 8), d::kCanvas);
    } else {
        ui::placeholderTile(fb, x, y, size, size, label, std::min(4, size / 8));
    }
}

// ---- header, sub-bar
// ------------------------------------------------------------------------------

void MusicScreen::renderSubBar(SDL_Surface* fb)
{
    ui::fill(fb, 0, kSubBarTop, d::kScreenW, kSubBarH, d::kCanvas);
    const TabRuntime& tab = m_tabs[m_activeTab];
    const MusicPane& pane = activePane();
    const int y = kSubBarTop + 6;
    int x = d::kMargin;
    if (!tab.drill.empty()) {
        // Breadcrumb: back chevron, the root section, then the page we are in.
        ui::iconChevron(fb, x, y + 2, 10, false, d::kAccentHi);
        x += 18;
        const MusicPane& root = tab.roots[tab.section];
        const char* section = root.frame.kind == MusicPaneKind::Artists     ? "Artists"
                              : root.frame.kind == MusicPaneKind::Albums    ? "Albums"
                              : root.frame.kind == MusicPaneKind::Songs     ? "Songs"
                              : root.frame.kind == MusicPaneKind::Playlists ? "Playlists"
                              : root.frame.kind == MusicPaneKind::Home      ? "Home"
                              : root.frame.kind == MusicPaneKind::Downloads ? "Downloads"
                                                                            : "";
        x += ui::text(fb, x, y, section, d::kTextMuted) + 8;
        x += ui::text(fb, x, y, "/", d::kTextMuted) + 8;
        ui::textClamped(fb, x, y, d::kScreenW - x - d::kMargin, pane.frame.title, d::kText);
    } else if (tab.roots.size() > 1) {
        const char* names[kLibrarySections] = {"Artists", "Albums", "Songs"};
        for (int i = 0; i < static_cast<int>(tab.roots.size()); ++i) {
            const bool active = i == tab.section;
            const int w = ui::text(fb, x, y, names[i], active ? d::kText : d::kTextMuted);
            if (active)
                gradientBar(fb, x, kSubBarTop + kSubBarH - 4, w, 2, w);
            x += w + 22;
        }
        ui::text(fb, x, y, "L2/R2", d::kTextMuted);
    } else {
        const char* title = pane.frame.kind == MusicPaneKind::Home        ? "Home"
                            : pane.frame.kind == MusicPaneKind::Playlists ? "Playlists"
                            : pane.frame.kind == MusicPaneKind::Downloads ? "Downloads"
                                                                          : "Settings";
        ui::text(fb, x, y, title, d::kText);
    }
    // Right side: offline marker, the letter filter and the count.
    int right = d::kScreenW - d::kMargin;
    if (m_session.manualOfflineMode) {
        right -= ui::textWidth("Offline") + 16;
        ui::chip(fb, right, y - 2, "Offline", d::kRaised, d::kWarning);
        right -= 10;
    }
    if (pane.total > 0 && pane.frame.kind != MusicPaneKind::Home) {
        const std::string count = std::to_string(pane.total);
        right -= ui::textWidth(count);
        ui::text(fb, right, y, count, d::kTextMuted);
        right -= 12;
    }
    if (pane.frame.letter) {
        const std::string chip = std::string(1, pane.frame.letter);
        right -= ui::textWidth(chip) + 16;
        ui::chip(fb, right, y - 2, chip, d::kAccentSoft, d::kAccentHi);
    }
}

void MusicScreen::renderDetailHeader(SDL_Surface* fb, const MusicPane& pane, int top)
{
    const MusicFrame& f = pane.frame;
    drawCover(fb, f.artId, f.artTag, d::kMargin, top + 6, 92, 128, f.title);
    const int x = d::kMargin + 92 + 14, w = d::kScreenW - x - d::kMargin;
    const auto lines = ui::wrap(f.title, w, 2, 1);
    int y = top + 8;
    for (const std::string& line : lines) {
        ui::text(fb, x, y, line, d::kText);
        y += 18;
    }
    if (!f.subtitle.empty()) {
        ui::textClamped(fb, x, y, w, f.subtitle, d::kTextSecondary);
        y += 18;
    }
    // Totals once the tracks are in.
    int tracks = 0;
    std::int64_t ticks = 0;
    for (const MusicRow& r : pane.rows) {
        if (r.kind == MusicRow::Kind::Track) {
            ++tracks;
            ticks += r.track.runTimeTicks;
        }
    }
    char meta[64] = "";
    if (f.kind == MusicPaneKind::ArtistAlbums)
        std::snprintf(meta, sizeof(meta), "%d %s", static_cast<int>(pane.rows.size()),
                      pane.rows.size() == 1 ? "album" : "albums");
    else if (tracks > 0)
        std::snprintf(meta, sizeof(meta), "%d %s - %d min", tracks,
                      tracks == 1 ? "track" : "tracks", static_cast<int>(ticks / 10000000 / 60));
    ui::text(fb, x, y, meta, d::kTextMuted);
    ui::fill(fb, d::kMargin, top + 104, d::kScreenW - 2 * d::kMargin, 1, d::kDivider);
}

// ---- lists
// ----------------------------------------------------------------------------------------

void MusicScreen::renderPane(SDL_Surface* fb, const MusicPane& pane, int top, int bottom)
{
    int listTop = top;
    const int header = detailHeaderHeight(pane);
    if (header > 0) {
        renderDetailHeader(fb, pane, top);
        listTop += header;
    }
    const int rowW = d::kScreenW - 2 * d::kMargin;
    if (pane.rows.empty()) {
        std::string line, detail;
        if (pane.frame.kind == MusicPaneKind::Downloads) {
            line = "No offline music yet";
            detail = "Press Y on an album or playlist to download it.";
        } else if (pane.failed) {
            line = "Couldn't load";
            detail = pane.error;
        } else if (pane.ticket != 0 || pane.ticket2 != 0 || !pane.requested) {
            line = "Loading...";
        } else {
            line = "Nothing here";
            detail = pane.frame.letter ? "No names start with this letter." : "";
        }
        const int cy = (listTop + bottom) / 2 - 16;
        ui::text(fb, (d::kScreenW - ui::textWidth(line)) / 2, cy, line, d::kTextSecondary);
        if (!detail.empty())
            ui::text(fb, (d::kScreenW - ui::textWidth(ui::fit(detail, 560))) / 2, cy + 22,
                     ui::fit(detail, 560), d::kTextMuted);
        return;
    }
    const int visible = std::max(1, (bottom - listTop) / kRowHeight);
    int y = listTop;
    for (int i = pane.frame.scroll; i < static_cast<int>(pane.rows.size()) && y + 26 <= bottom;
         ++i) {
        const MusicRow& row = pane.rows[i];
        if (row.kind == MusicRow::Kind::Heading) {
            ui::text(fb, d::kMargin + 4, y + 8, row.title, d::kAccentHi);
            ui::fill(fb, d::kMargin + 4 + ui::textWidth(row.title) + 8, y + 15,
                     rowW - ui::textWidth(row.title) - 20, 1, d::kDivider);
            y += 26;
            continue;
        }
        if (y + kRowHeight > bottom + 2)
            break;
        const bool selected = i == pane.frame.selected;
        if (selected) {
            ui::roundFill(fb, d::kMargin, y, rowW, kRowHeight - 4, d::kRadius, d::kRaised);
            for (int k = 0; k < kRowHeight - 12; ++k)
                ui::fill(fb, d::kMargin, y + 4 + k, 3, 1,
                         ui::mix(d::kAccent, d::kAccentHi, k * 100 / (kRowHeight - 13)));
        }
        const int thumb = kRowHeight - 10;
        int textX = d::kMargin + 10;
        if (row.kind != MusicRow::Kind::Action || !row.artId.empty()) {
            drawCover(fb, row.artId, row.artTag, d::kMargin + 10, y + 1, thumb, 128, row.title);
            textX += thumb + 10;
        }
        int rightEdge = d::kMargin + rowW - 10;
        if (!row.right.empty()) {
            const int rw = ui::textWidth(row.right);
            ui::text(fb, rightEdge - rw, y + 12, row.right,
                     selected ? d::kTextSecondary : d::kTextMuted);
            rightEdge -= rw + 12;
        }
        if (isDownloaded(row) && row.kind != MusicRow::Kind::Action) {
            ui::iconDownload(fb, rightEdge - 12, y + 13, 12, d::kAccentHi);
            rightEdge -= 20;
        }
        const bool playing = row.kind == MusicRow::Kind::Track && m_player &&
                             m_playerView.track.id == row.track.id &&
                             m_playerView.state != music::PlayState::Idle;
        const d::Rgb titleColor = playing ? d::kAccentHi : (selected ? d::kText : d::kText);
        if (row.subtitle.empty()) {
            ui::textClamped(fb, textX, y + 12, rightEdge - textX, row.title, titleColor);
        } else {
            ui::textClamped(fb, textX, y + 3, rightEdge - textX, row.title, titleColor);
            ui::textClamped(fb, textX, y + 21, rightEdge - textX, row.subtitle,
                            selected ? d::kTextSecondary : d::kTextMuted);
        }
        if (row.progress >= 0) {
            const int barW = rowW - (textX - d::kMargin) - 12;
            ui::fill(fb, textX, y + kRowHeight - 9, barW, 3, d::kDivider);
            gradientBar(fb, textX, y + kRowHeight - 9, barW, 3, barW * row.progress / 100);
        }
        y += kRowHeight;
    }
    // Scroll indicator.
    const int total = static_cast<int>(pane.rows.size());
    if (total > visible) {
        const int trackH = bottom - listTop - 4;
        const int thumbH = std::max(14, trackH * visible / total);
        const int thumbY =
            listTop + (trackH - thumbH) * pane.frame.scroll / std::max(1, total - visible);
        ui::fill(fb, d::kScreenW - 8, listTop, 3, trackH, d::kDivider);
        ui::roundFill(fb, d::kScreenW - 8, thumbY, 3, thumbH, 1, d::kAccentDim);
    }
}

// ---- mini player, now playing
// ---------------------------------------------------------------------

void MusicScreen::renderMiniPlayer(SDL_Surface* fb, int top)
{
    const music::PlayerView& v = m_playerView;
    ui::fill(fb, 0, top, d::kScreenW, kMiniH, d::kPanel);
    ui::fill(fb, 0, top, d::kScreenW, 1, d::kBorder);
    drawCover(fb, v.track.artId(), v.track.artTag(), d::kMargin, top + 4, 44, 128, v.track.title);
    const int x = d::kMargin + 44 + 12;
    const int right = d::kScreenW - d::kMargin - 28;
    ui::textClamped(fb, x, top + 6, right - x, v.track.title, d::kText);
    std::string sub = v.track.artist;
    if (v.state == music::PlayState::Loading)
        sub = "Loading...";
    else if (!v.message.empty())
        sub = v.message;
    ui::textClamped(fb, x, top + 24, right - x, sub, d::kTextSecondary);
    // Progress.
    const int barX = x, barW = right - x, barY = top + 44;
    ui::fill(fb, barX, barY, barW, 4, d::kDivider);
    if (v.duration > 0)
        gradientBar(fb, barX, barY, barW, 4,
                    static_cast<int>(barW * std::min(1.0, v.position / v.duration)));
    // Play / pause glyph.
    const int gx = d::kScreenW - d::kMargin - 18, gy = top + 14;
    if (v.state == music::PlayState::Paused) {
        ui::iconPlay(fb, gx, gy, 16, d::kText);
    } else if (v.state == music::PlayState::Playing) {
        ui::fill(fb, gx + 2, gy, 4, 16, d::kText);
        ui::fill(fb, gx + 10, gy, 4, 16, d::kText);
    } else {
        ui::statusDot(fb, gx + 4, gy + 4, 8, d::kAccent);
    }
}

void MusicScreen::renderNowPlaying(SDL_Surface* fb)
{
    ui::fill(fb, 0, d::kHeaderH, d::kScreenW, d::kScreenH - d::kHeaderH - d::kFooterH, d::kCanvas);
    const music::PlayerView& v = m_playerView;
    const int top = d::kHeaderH + 14;
    if (!m_queueView) {
        drawCover(fb, v.track.artId(), v.track.artTag(), 24, top, 216, 256, v.track.title);
        const int x = 268, w = d::kScreenW - x - 20;
        const auto lines = ui::wrap(v.track.title, w / 2, 2, 2);
        int y = top;
        for (const std::string& line : lines) {
            ui::text(fb, x, y, line, d::kText, 2);
            y += 34;
        }
        if (!v.track.artist.empty()) {
            ui::textClamped(fb, x, y + 2, w, v.track.artist, d::kTextSecondary);
            y += 22;
        }
        if (!v.track.album.empty())
            ui::textClamped(fb, x, y + 2, w, v.track.album, d::kTextMuted);
        // Progress bar and times.
        const int barY = top + 152;
        ui::fill(fb, x, barY, w, 6, d::kDivider);
        if (v.duration > 0)
            gradientBar(fb, x, barY, w, 6,
                        static_cast<int>(w * std::min(1.0, v.position / v.duration)));
        ui::text(fb, x, barY + 12, clock(v.position), d::kTextSecondary);
        const std::string total = clock(v.duration);
        ui::text(fb, x + w - ui::textWidth(total), barY + 12, total, d::kTextMuted);
        // Transport.
        const int cy = barY + 44, cx = x + w / 2;
        ui::iconChevron(fb, cx - 70, cy, 18, false, d::kText);
        ui::iconChevron(fb, cx - 80, cy, 18, false, d::kText);
        if (v.state == music::PlayState::Playing) {
            ui::fill(fb, cx - 11, cy - 4, 7, 26, d::kText);
            ui::fill(fb, cx + 4, cy - 4, 7, 26, d::kText);
        } else {
            ui::iconPlay(fb, cx - 12, cy - 4, 26, d::kText);
        }
        ui::iconChevron(fb, cx + 52, cy, 18, true, d::kText);
        ui::iconChevron(fb, cx + 62, cy, 18, true, d::kText);
        // Shuffle / repeat.
        int cxChip = x;
        cxChip += ui::chip(fb, cxChip, barY + 92, v.shuffle ? "Shuffle on" : "Shuffle off",
                           v.shuffle ? d::kAccentDim : d::kRaised,
                           v.shuffle ? d::kText : d::kTextSecondary) +
                  8;
        const char* repeat = v.repeat == music::Repeat::One   ? "Repeat one"
                             : v.repeat == music::Repeat::All ? "Repeat all"
                                                              : "Repeat off";
        ui::chip(fb, cxChip, barY + 92, repeat,
                 v.repeat == music::Repeat::Off ? d::kRaised : d::kAccentDim,
                 v.repeat == music::Repeat::Off ? d::kTextSecondary : d::kText);
        if (const music::Track* next = m_player->queue().peekNext()) {
            ui::text(fb, x, barY + 126, "Up next", d::kAccentHi);
            ui::textClamped(fb, x + 72, barY + 126, w - 72, next->title, d::kTextSecondary);
        }
        if (!v.message.empty())
            ui::textClamped(fb, x, barY + 150, w, v.message, d::kWarning);
    } else {
        // Queue view: play order, current track marked.
        const music::MusicQueue& q = m_player->queue();
        ui::text(fb, d::kMargin, top - 4, "Queue", d::kAccentHi);
        ui::text(fb, d::kScreenW - d::kMargin - ui::textWidth(std::to_string(q.size()) + " tracks"),
                 top - 4, std::to_string(q.size()) + " tracks", d::kTextMuted);
        const int rowsVisible = (d::kScreenH - d::kFooterH - top - 24) / 36;
        const int first =
            std::max(0, std::min(m_queueSelected - rowsVisible / 2, q.size() - rowsVisible));
        for (int i = 0; i < rowsVisible && first + i < q.size(); ++i) {
            const music::Track* t = q.at(first + i);
            const int y = top + 20 + i * 36;
            const bool selected = first + i == m_queueSelected;
            if (selected)
                ui::roundFill(fb, d::kMargin, y, d::kScreenW - 2 * d::kMargin, 32, d::kRadius,
                              d::kRaised);
            const bool current = first + i == q.position();
            ui::text(fb, d::kMargin + 10, y + 8, std::to_string(first + i + 1) + ".",
                     d::kTextMuted);
            ui::textClamped(fb, d::kMargin + 50, y + 8, 400, t->title,
                            current ? d::kAccentHi : d::kText);
            ui::textClamped(fb, d::kMargin + 460, y + 8, 120, t->artist, d::kTextMuted);
        }
    }
}

// ---- menu, toast, footer
// --------------------------------------------------------------------------

void MusicScreen::renderMenu(SDL_Surface* fb)
{
    ui::blend(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas, 190);
    const int count = static_cast<int>(m_menu.items.size());
    const int w = 280, h = 52 + count * 32;
    const int x = (d::kScreenW - w) / 2, y = (d::kScreenH - h) / 2;
    ui::panel(fb, x, y, w, h);
    ui::textClamped(fb, x + 16, y + 12, w - 32, m_menu.row.title, d::kText);
    for (int i = 0; i < count; ++i) {
        const int ry = y + 40 + i * 32;
        const bool selected = i == m_menu.selected;
        if (selected)
            ui::focusRing(fb, x + 10, ry, w - 20, 28);
        ui::roundFill(fb, x + 10, ry, w - 20, 28, d::kRadius, selected ? d::kRaised : d::kPanel);
        ui::text(fb, x + 24, ry + 6, m_menu.items[i].label,
                 selected ? d::kAccentHi : d::kTextSecondary);
    }
}

void MusicScreen::renderFooter(SDL_Surface* fb, bool miniShown)
{
    ui::FooterSpec footer;
    footer.showLink = false;
    if (m_view == View::NowPlaying) {
        if (m_queueView)
            footer.hints = {{ui::Key::A, "Play"},
                            {ui::Key::Y, "Remove"},
                            {ui::Key::X, "Save as playlist"},
                            {ui::Key::B, "Back"}};
        else
            footer.hints = {{ui::Key::A, "Pause"},
                            {ui::Key::Dpad, "Seek"},
                            {ui::Key::L2, "Skip"},
                            {ui::Key::X, "Shuffle"},
                            {ui::Key::Y, "Repeat"}};
    } else if (m_picker.active()) {
        footer.hints = {{ui::Key::A, "Select"}, {ui::Key::B, "Close"}};
    } else if (m_menu.open) {
        footer.hints = {{ui::Key::A, "Select"}, {ui::Key::B, "Close"}};
    } else {
        footer.hints = {{ui::Key::Dpad, "Move"}, {ui::Key::A, "Select"}, {ui::Key::B, "Back"}};
        const MusicPane& pane = activePane();
        if (pane.frame.selected >= 0 && pane.frame.selected < static_cast<int>(pane.rows.size()) &&
            pane.rows[pane.frame.selected].kind != MusicRow::Kind::Action &&
            pane.rows[pane.frame.selected].kind != MusicRow::Kind::Artist)
            footer.hints.push_back({ui::Key::Y, "Options"});
        if (isAlphabetical(pane.frame.kind))
            footer.hints[0].label = "Move, A-Z";
        if (miniShown)
            footer.hints.push_back({ui::Key::Start, "Now playing"});
    }
    if (!m_toast.empty()) {
        footer.message = m_toast;
        footer.messageColor = d::kAccentHi;
    }
    ui::footer(fb, footer);
}

// ---- screen
// ---------------------------------------------------------------------------------------

void MusicScreen::render(SDL_Surface* fb)
{
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.tabs = tabNames();
    header.activeTab = m_activeTab;
    header.batteryPercent = m_battery.percent();
    header.charging = m_battery.charging();
    header.tabPad = 5;
    header.tabGap = 4;
    ui::header(fb, header);

    const bool mini = miniPlayerVisible();
    if (m_view == View::NowPlaying && mini) {
        renderNowPlaying(fb);
        renderFooter(fb, mini);
        return;
    }
    renderSubBar(fb);
    renderPane(fb, activePane(), 76, contentBottom());
    if (mini)
        renderMiniPlayer(fb, kMiniTop);
    renderFooter(fb, mini);
    if (m_menu.open)
        renderMenu(fb);
    m_picker.render(fb);
}

} // namespace miyoofin
