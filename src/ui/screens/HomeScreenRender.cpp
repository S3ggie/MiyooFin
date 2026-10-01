#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../ArtworkPresentation.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
#include "../UiKit.hpp"
#include "../../data/MovieTitle.hpp"
#include "../ShowsBrowser.hpp"
#include "../../diagnostics/UiDiagnostics.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include "../../download/DownloadSupport.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace miyoofin {

namespace {

static constexpr std::int64_t SYNC_FRESH_WALL_MS = 15LL * 60 * 1000;
static constexpr std::int64_t HIERARCHY_RECONCILE_MS = 24LL * 60 * 60 * 1000;

namespace d = design;

// Home rails: heading row, gap to the cards, caption block, gap between rails.
constexpr int RAIL_TOP = d::kHeaderH + 12;
constexpr int RAIL_HEADING_H = 24;
constexpr int RAIL_CAPTION_GAP = 6;
constexpr int RAIL_CAPTION_H = 32;
constexpr int RAIL_GAP = 14;
constexpr int VISIBLE_RAILS = 2;

// Movies / Shows grids: a slim alphabet rail on the left, an info bar for the
// selected title, then 64x96 poster cards (columns and rows match navigation).
constexpr int MOVIE_GRID_COLUMNS = 8;
constexpr int MOVIE_GRID_ROWS = 3;
constexpr int GRID_CARD_W = 64;
constexpr int GRID_CARD_H = 96;
constexpr int GRID_GAP = 8;
constexpr int ALPHA_X = 8;
constexpr int ALPHA_W = 28;
constexpr int BROWSE_X = 44;                         // content left of the alphabet rail
constexpr int BROWSE_W = d::kScreenW - BROWSE_X - 8; // 588
constexpr int INFO_Y = d::kHeaderH + 8;
constexpr int INFO_H = 56;
constexpr int GRID_TOP = INFO_Y + INFO_H + 8;
constexpr int SHOWS_HALF_W = 288;
constexpr int SHOWS_LABEL_Y = GRID_TOP;
constexpr int SHOWS_GRID_TOP = GRID_TOP + 22;
constexpr int SHOWS_ROW_PITCH = GRID_CARD_H + 8;

// Downloads list.
constexpr int DL_SUMMARY_Y = d::kHeaderH + 8;
constexpr int DL_SUMMARY_H = 44;
constexpr int DL_LIST_TOP = DL_SUMMARY_Y + DL_SUMMARY_H + 8;
constexpr int DL_ROW_H = 60;
constexpr int DL_ROW_PITCH = 68;

// Footer text for the connection state next to "<user> ...".
void linkPresentation(const std::string& user, LinkStatus status, std::string& text,
                      ui::LinkState& state)
{
    const std::string who = user.empty() ? std::string("Server") : user;
    switch (status) {
    case LinkStatus::Connected:
        text = who + " connected";
        state = ui::LinkState::Connected;
        break;
    case LinkStatus::Offline:
        text = who + " offline";
        state = ui::LinkState::Offline;
        break;
    case LinkStatus::SessionExpired:
        text = who + " signed out";
        state = ui::LinkState::Checking;
        break;
    case LinkStatus::OfflineMode:
        text = who + " offline mode";
        state = ui::LinkState::Checking;
        break;
    case LinkStatus::Checking:
    default:
        text = who + " connecting...";
        state = ui::LinkState::Checking;
        break;
    }
}

// Small circular "watched" badge in the top-right corner of a card.
void drawWatchedBadge(SDL_Surface* fb, int x, int y, int w)
{
    const int size = 16;
    const int bx = x + w - size - 5;
    const int by = y + 5;
    ui::roundFill(fb, bx, by, size, size, size / 2, d::kSuccess);
    ui::iconCheck(fb, bx + 4, by + 4, 9, d::kCanvas);
}

// Scrim + progress bar along the lower edge of a card with watch progress.
void drawProgressOverlay(SDL_Surface* fb, int x, int y, int w, int h, const MediaItem& item)
{
    const bool hasProgress = item.progress > 0.0f || item.playbackPositionTicks > 0;
    if (item.played || !hasProgress)
        return;
    const int pct = std::max(1, std::min(100, playbackPercent(item)));
    ui::bottomScrim(fb, x, y, w, h, 26, 200);
    ui::progressBar(fb, x + 8, y + h - 12, w - 16, 4, pct, d::kAccent);
}

// Heading with a short accent bar, used for rails and grid sections.
void drawSectionHeading(SDL_Surface* fb, int x, int y, const std::string& label, bool focused,
                        const std::string& count = std::string())
{
    ui::fill(fb, x, y + 2, 3, 12, focused ? d::kAccent : d::kAccentDim);
    const int w = ui::text(fb, x + 10, y, label, focused ? d::kText : d::kTextSecondary);
    if (!count.empty())
        ui::text(fb, x + 10 + w + 8, y, count, d::kTextMuted);
}

// Two-line caption under a card.
void drawCardCaption(SDL_Surface* fb, int x, int y, int w, const MediaItem& item, bool focused)
{
    std::string title = item.title;
    std::string subtitle;
    if (item.type == "episode" && !item.seriesName.empty()) {
        title = item.seriesName;
        subtitle = item.title;
    } else if (item.year > 0) {
        subtitle = std::to_string(item.year);
    }
    ui::textClamped(fb, x, y, w, title, focused ? d::kText : d::kTextSecondary);
    if (!subtitle.empty())
        ui::textClamped(fb, x, y + BitmapFont::GLYPH_H, w, subtitle,
                        focused ? d::kAccentHi : d::kTextMuted);
}

// The details of the selected movie/show: a title and a row of chips.
void drawInfoBar(SDL_Surface* fb, const MediaItem* item, bool show)
{
    ui::panel(fb, BROWSE_X, INFO_Y, BROWSE_W, INFO_H);
    if (!item) {
        ui::text(fb, BROWSE_X + 14, INFO_Y + (INFO_H - 16) / 2, "Nothing selected", d::kTextMuted);
        return;
    }
    ui::textClamped(fb, BROWSE_X + 14, INFO_Y + 8, BROWSE_W - 28, item->title, d::kText);
    int x = BROWSE_X + 14;
    const int y = INFO_Y + 29;
    if (item->year > 0)
        x += ui::chip(fb, x, y, std::to_string(item->year), d::kRaised, d::kTextSecondary) + 6;
    const int mins = show ? 0 : ticksToMinutes(item->runTimeTicks);
    if (mins > 0) {
        char runtime[24];
        std::snprintf(runtime, sizeof(runtime), "%dh %dm", mins / 60, mins % 60);
        x += ui::chip(fb, x, y, runtime, d::kRaised, d::kTextSecondary) + 6;
    }
    if (item->rating > 0) {
        char rating[16];
        std::snprintf(rating, sizeof(rating), "%.1f", static_cast<double>(item->rating));
        x += ui::chip(fb, x, y, rating, d::kAccentSoft, d::kAccentHi) + 6;
    }
    if (!item->genre.empty())
        x += ui::chip(fb, x, y, item->genre, d::kRaised, d::kTextSecondary) + 6;
    if (item->played) {
        ui::chip(fb, x, y, "Watched", ui::mix(d::kCanvas, d::kSuccess, 22), d::kSuccess);
    } else if (item->progress > 0) {
        char state[24];
        std::snprintf(state, sizeof(state), "%d%% watched",
                      static_cast<int>(item->progress * 100.0f + 0.5f));
        ui::chip(fb, x, y, state, d::kAccentSoft, d::kAccentHi);
    }
}

// Slim A-Z rail. `focus` is the focused letter when the rail itself has focus
// (otherwise -1); `active` is the letter currently filtering the grid.
void drawAlphabetRail(SDL_Surface* fb, int focus, int active)
{
    ui::panel(fb, ALPHA_X, INFO_Y, ALPHA_W, 480 - d::kFooterH - 8 - INFO_Y);
    const int pitch = 15;
    const int top = INFO_Y + 5;
    for (int i = 0; i < 26; ++i) {
        const int y = top + i * pitch;
        const bool focused = i == focus;
        const bool isActive = i == active;
        if (focused)
            ui::roundFill(fb, ALPHA_X + 3, y, ALPHA_W - 6, pitch, 3, d::kAccent);
        if (isActive && !focused)
            ui::fill(fb, ALPHA_X + 2, y + 3, 2, pitch - 6, d::kAccentHi);
        const char letter[2] = {static_cast<char>('A' + i), '\0'};
        ui::text(fb, ALPHA_X + (ALPHA_W - 8) / 2, y - 1, letter,
                 focused ? d::Rgb{255, 255, 255} : (isActive ? d::kAccentHi : d::kTextMuted));
    }
}

// Centered empty-state / loading panel.
void drawCenteredNote(SDL_Surface* fb, const std::string& title, const std::string& detail,
                      d::Rgb titleColor)
{
    const int w = 360;
    const int h = detail.empty() ? 64 : 92;
    const int x = (d::kScreenW - w) / 2;
    const int y = (d::kHeaderH + (d::kScreenH - d::kFooterH - d::kHeaderH - h)) / 2;
    ui::panel(fb, x, y, w, h);
    ui::text(fb, x + (w - ui::textWidth(ui::fit(title, w - 24))) / 2, y + 18,
             ui::fit(title, w - 24), titleColor);
    if (!detail.empty()) {
        const auto lines = ui::wrap(detail, w - 32, 2);
        int ly = y + 44;
        for (const std::string& line : lines) {
            ui::text(fb, x + (w - ui::textWidth(line)) / 2, ly, line, d::kTextSecondary);
            ly += 18;
        }
    }
}

} // namespace

void HomeScreen::render(SDL_Surface* fb)
{
    if (m_loadState == LoadState::Ready && !m_firstInteractiveFrameLogged) {
        m_firstInteractiveFrameLogged = true;
        uiDiagnostics().log("[HomeScreen] startup stage=first_interactive_frame");
    }
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    drawTabBar(fb);

    if (m_loadState == LoadState::Loading) {
        drawLoadingState(fb);
        drawBottomHints(fb);
        return;
    }
    if (m_loadState == LoadState::Error) {
        drawErrorState(fb);
        drawBottomHints(fb);
        return;
    }

    // Ready
    const TabData& tab = currentTab();
    if (activeTabNamed("Downloads")) {
        drawDownloadsTab(fb);
    } else if (activeTabNamed("Settings")) {
        drawSettingsTab(fb);
    } else if (!activeTabNamed("Movies") && !activeTabNamed("Shows") && tab.rows.size() == 1 &&
               tab.rows[0].items.empty()) {
        drawPlaceholderTab(fb,
                           tab.rows[0].label.empty() ? "No content" : tab.rows[0].label.c_str());
    } else {
        bool hasItems = false;
        for (const auto& r : tab.rows) {
            if (!r.items.empty()) {
                hasItems = true;
                break;
            }
        }
        if (activeTabNamed("Movies")) {
            drawMoviePreview(fb);
            drawMovieAlphabetRail(fb);
            drawMovieGrid(fb);
        } else if (activeTabNamed("Shows")) {
            drawShowsPreview(fb);
            drawShowsAlphabetRail(fb);
            drawShowsGrid(fb);
        } else if (!hasItems) {
            drawPlaceholderTab(fb, tab.name == "Movies"  ? "No movies on this server"
                                   : tab.name == "Shows" ? "No shows on this server"
                                                         : "No content");
        } else {
            drawRowList(fb);
        }
    }
    drawBottomHints(fb);
}

void HomeScreen::drawTabBar(SDL_Surface* fb)
{
    ui::HeaderSpec spec;
    for (const TabData& tab : m_tabs)
        spec.tabs.push_back(tab.name);
    spec.activeTab = m_activeTab;
    spec.batteryPercent = m_battery.percent();
    spec.charging = m_battery.charging();
    ui::header(fb, spec);
}

std::string HomeScreen::syncStatusText() const
{
    if (!activeTabNamed("Home") && !activeTabNamed("Movies") && !activeTabNamed("Shows"))
        return "";
    if (m_homeSyncActive)
        return homeSyncStatus(true);
    const auto artworkProgress = m_artworkController ? m_artworkController->artworkProgress()
                                                     : HomeArtworkController::ArtworkProgress{};
    const std::string artworkStatus = artworkSyncStatus(
        artworkProgress.active, m_libraryFetch && m_libraryFetch->artworkPlanningComplete(),
        ShowsSyncProgress{artworkProgress.completed, artworkProgress.total});
    if (!artworkStatus.empty())
        return artworkStatus;
    const std::size_t hierarchyCompleted = m_hierarchyCompleted.load();
    const std::size_t hierarchyTotal = m_hierarchyTotal.load();
    const bool hierarchyInProgress =
        hierarchyTotal != 0 && (m_hierarchyActive.load() || hierarchyCompleted < hierarchyTotal);
    const ShowsSyncProgress progress =
        hierarchyInProgress
            ? ShowsSyncProgress{hierarchyCompleted, hierarchyTotal}
            : ShowsSyncProgress{m_libraryFetch ? m_libraryFetch->metadataCompleted() : 0,
                                m_libraryFetch ? m_libraryFetch->metadataTotal() : 0};
    return librarySyncStatus(
        m_activeTab, m_haveCachedSnapshot, m_libraryOffline || m_hierarchyOffline.load(),
        m_syncSchedule.inFlight || (m_libraryFetch && m_libraryFetch->metadataActive()),
        m_syncSchedule.hasSucceeded, progress,
        m_hierarchyActive.load() || (m_libraryFetch && m_libraryFetch->metadataActive()),
        activeTabNamed("Shows"));
}

void HomeScreen::drawRowList(SDL_Surface* fb)
{
    const auto& rows = currentTab().rows;
    if (rows.empty())
        return;
    int railY = RAIL_TOP;
    for (int ri = 0; ri < VISIBLE_RAILS; ++ri) {
        const int rowIdx = m_rowScroll + ri;
        if (rowIdx >= static_cast<int>(rows.size()))
            break;
        const MediaRow& row = rows[rowIdx];
        const bool rowFocused = rowIdx == m_activeRow;
        const bool landscape = homeRailIsLandscape(row.label);
        const ArtworkBox box = homeRailCardSize(landscape);

        drawSectionHeading(fb, HOME_RAIL_MARGIN, railY, row.label, rowFocused);
        const int cardsY = railY + RAIL_HEADING_H;
        const int captionY = cardsY + box.h + RAIL_CAPTION_GAP;

        // Cards with a pixel-scroll offset on the focused rail.
        int cardAccumX = HOME_RAIL_MARGIN;
        for (int ci = 0; ci < static_cast<int>(row.items.size()); ++ci) {
            const MediaItem& item = row.items[ci];
            const int screenX = cardAccumX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            cardAccumX += box.w + HOME_RAIL_GAP;
            if (screenX + box.w < 0)
                continue; // scrolled off the left edge
            if (screenX > d::kScreenW)
                break;
            const bool sel = rowFocused && ci == m_activeCard;
            drawCard(fb, screenX, cardsY, box.w, box.h, item, sel, CardPresentation::Home);
            if (landscape)
                drawProgressOverlay(fb, screenX, cardsY, box.w, box.h, item);
            drawCardCaption(fb, screenX, captionY, box.w, item, sel);
        }
        railY = captionY + RAIL_CAPTION_H + RAIL_GAP;
    }
}

void HomeScreen::drawMovieGrid(SDL_Surface* fb)
{
    const MediaRow* row = currentRow();
    if (!row)
        return;
    const int gridW = MOVIE_GRID_COLUMNS * GRID_CARD_W + (MOVIE_GRID_COLUMNS - 1) * GRID_GAP;
    const int left = BROWSE_X + (BROWSE_W - gridW) / 2;
    for (int i = 0; i < static_cast<int>(row->items.size()); ++i) {
        const int gr = i / MOVIE_GRID_COLUMNS, gc = i % MOVIE_GRID_COLUMNS;
        if (gr < m_rowScroll || gr >= m_rowScroll + MOVIE_GRID_ROWS)
            continue;
        drawCard(fb, left + gc * (GRID_CARD_W + GRID_GAP),
                 GRID_TOP + (gr - m_rowScroll) * (GRID_CARD_H + GRID_GAP), GRID_CARD_W, GRID_CARD_H,
                 row->items[i], !m_movieRailFocused && i == m_activeCard);
    }
    if (row->items.empty() && m_movieActiveLetter >= 0) {
        char message[48];
        std::snprintf(message, sizeof(message), "No movies starting with %c",
                      'A' + m_movieActiveLetter);
        drawCenteredNote(fb, message, "Pick another letter, or press A on the letter again.",
                         d::kTextSecondary);
    }
}

void HomeScreen::drawMovieAlphabetRail(SDL_Surface* fb)
{
    drawAlphabetRail(fb, m_movieRailFocused ? m_movieAlphabetFocus : -1, m_movieActiveLetter);
}

void HomeScreen::drawMoviePreview(SDL_Surface* fb)
{
    drawInfoBar(fb, currentItem(), false);
}

void HomeScreen::drawShowsAlphabetRail(SDL_Surface* fb)
{
    drawAlphabetRail(fb, m_showsFocus == ShowsFocus::AlphabetRail ? m_showsAlphabetFocus : -1,
                     m_showsActiveLetter);
}

void HomeScreen::drawShowsPreview(SDL_Surface* fb)
{
    drawInfoBar(fb, showsSelectedItem(), true);
}

void HomeScreen::drawShowsGrid(SDL_Surface* fb)
{
    const int leftX = BROWSE_X;
    const int rightX = BROWSE_X + SHOWS_HALF_W + 12;
    drawSectionHeading(fb, leftX, SHOWS_LABEL_Y, "Shows", m_showsFocus == ShowsFocus::ShowsGrid,
                       std::to_string(m_filteredShows.size()));
    drawSectionHeading(fb, rightX, SHOWS_LABEL_Y, "Anime", m_showsFocus == ShowsFocus::AnimeGrid,
                       std::to_string(m_filteredAnime.size()));
    ui::fill(fb, BROWSE_X + SHOWS_HALF_W + 5, SHOWS_LABEL_Y, 1,
             480 - d::kFooterH - 8 - SHOWS_LABEL_Y, d::kDivider);

    auto draw = [&](const std::vector<MediaItem>& items, int scroll, int selected, bool focused,
                    int base) {
        const int gridW = 4 * GRID_CARD_W + 3 * GRID_GAP;
        const int left = base + (SHOWS_HALF_W - gridW) / 2;
        for (int i = 0; i < static_cast<int>(items.size()); ++i) {
            const int row = i / 4;
            if (row < scroll || row >= scroll + 3)
                continue;
            drawCard(fb, left + (i % 4) * (GRID_CARD_W + GRID_GAP),
                     SHOWS_GRID_TOP + (row - scroll) * SHOWS_ROW_PITCH, GRID_CARD_W, GRID_CARD_H,
                     items[i], focused && i == selected);
        }
    };
    draw(m_filteredShows, m_showScroll, m_showSelected, m_showsFocus == ShowsFocus::ShowsGrid,
         leftX);
    draw(m_filteredAnime, m_animeScroll, m_animeSelected, m_showsFocus == ShowsFocus::AnimeGrid,
         rightX);
    if (m_filteredShows.empty() && m_filteredAnime.empty()) {
        char b[64];
        if (m_showsActiveLetter >= 0)
            std::snprintf(b, sizeof(b), "No shows starting with %c", 'A' + m_showsActiveLetter);
        else
            std::snprintf(b, sizeof(b), "No shows on this server");
        drawCenteredNote(fb, b, m_showsActiveLetter >= 0 ? "Pick another letter." : "",
                         d::kTextSecondary);
    }
}

void HomeScreen::drawCard(SDL_Surface* fb, int x, int y, int w, int h, const MediaItem& item,
                          bool selected, CardPresentation presentation)
{
    (void)presentation; // Home rails and grids share one card design now
    const int radius = w >= 100 ? 5 : 4;

    bool drawn = false;
    const std::string key = rowArtworkKey(item);
    if (!key.empty()) {
        auto it = m_rowArtworkCache.entries.find(key);
        if (it != m_rowArtworkCache.entries.end() &&
            it->second.status == RowArtworkStatus::Loaded && it->second.image &&
            !it->second.image->empty()) {
            char cacheKeyBuf[512];
            std::snprintf(cacheKeyBuf, sizeof(cacheKeyBuf), "%s:%dx%d", key.c_str(), w, h);
            const std::string cacheKey(cacheKeyBuf);
            auto cached = m_rowArtworkCache.cardSurfaces.find(cacheKey);
            if (cached == m_rowArtworkCache.cardSurfaces.end() || !cached->second) {
                prepareCardSurface(cacheKey, *it->second.image, w, h);
                cached = m_rowArtworkCache.cardSurfaces.find(cacheKey);
            }
            if (cached != m_rowArtworkCache.cardSurfaces.end() && cached->second) {
                SDL_Rect dst = {x, y, cached->second->w, cached->second->h};
                SDL_BlitSurface(cached->second, nullptr, fb, &dst);
                drawn = true;
            }
        }
    }
    if (!drawn)
        ui::placeholderTile(fb, x, y, w, h, item.title, radius);
    ui::roundCorners(fb, x, y, w, h, radius, d::kCanvas);

    if (item.played)
        drawWatchedBadge(fb, x, y, w);
    else if (w < 100)
        drawProgressOverlay(fb, x, y, w, h, item); // grid cards; rails draw their own

    if (selected)
        ui::focusRing(fb, x, y, w, h, radius);
    else
        ui::roundOutline(fb, x, y, w, h, radius, d::kBorder);
}

void HomeScreen::drawPlaceholderTab(SDL_Surface* fb, const char* message)
{
    drawCenteredNote(fb, message, "", d::kTextSecondary);
}

void HomeScreen::drawDownloadsTab(SDL_Surface* fb)
{
    const DownloadSnapshot& snap = m_downloadsState.snapshot;

    // Storage summary: three figures in one panel.
    ui::panel(fb, d::kMargin, DL_SUMMARY_Y, d::kScreenW - 2 * d::kMargin, DL_SUMMARY_H);
    struct Stat
    {
        const char* label;
        std::string value;
        d::Rgb color;
    };
    const Stat stats[3] = {{"FREE", formatBytes(snap.freeBytes), d::kText},
                           {"DOWNLOADED", formatBytes(snap.localBytes), d::kText},
                           {"QUEUED", formatBytes(snap.reservedBytes),
                            snap.reservedBytes > 0 ? d::kAccentHi : d::kTextSecondary}};
    const int colW = (d::kScreenW - 2 * d::kMargin) / 3;
    for (int i = 0; i < 3; ++i) {
        const int cx = d::kMargin + 14 + i * colW;
        ui::text(fb, cx, DL_SUMMARY_Y + 6, stats[i].label, d::kTextMuted);
        ui::text(fb, cx, DL_SUMMARY_Y + 24, stats[i].value, stats[i].color);
        if (i > 0)
            ui::fill(fb, d::kMargin + i * colW, DL_SUMMARY_Y + 8, 1, DL_SUMMARY_H - 16,
                     d::kDivider);
    }

    const auto& rows = m_downloadsState.hierarchy.visible;
    if (rows.empty()) {
        drawCenteredNote(fb, "No downloads yet",
                         "Open a movie or episode and choose Download to watch it offline.",
                         d::kTextSecondary);
        if (!m_downloadsState.missingJournal.empty()) {
            ui::panel(fb, d::kMargin, d::kScreenH - d::kFooterH - 52, d::kScreenW - 2 * d::kMargin,
                      40, ui::mix(d::kPanel, d::kWarning, 14), d::kWarning);
            ui::text(fb, d::kMargin + 14, d::kScreenH - d::kFooterH - 40,
                     "Missing offline progress: press X to discard", d::kWarning);
        }
        return;
    }

    for (int visible = 0; visible < HomeDownloadsState::kVisibleRows; ++visible) {
        const int index = m_downloadsState.scroll + visible;
        if (index >= static_cast<int>(rows.size()))
            break;
        const DownloadHierarchyRow& row = rows[index];
        const DownloadItem* item = row.item;
        const bool selected = index == m_downloadsState.selected;
        const int indent = row.indent * 14;
        const int rx = d::kMargin + indent;
        const int rw = d::kScreenW - 2 * d::kMargin - indent;
        const int ry = DL_LIST_TOP + visible * DL_ROW_PITCH;

        if (selected)
            ui::focusRing(fb, rx, ry, rw, DL_ROW_H);
        ui::roundFill(fb, rx, ry, rw, DL_ROW_H, d::kRadius, selected ? d::kRaised : d::kPanel);
        ui::roundOutline(fb, rx, ry, rw, DL_ROW_H, d::kRadius, selected ? d::kAccent : d::kBorder);

        // Kind chip, then the title.
        const bool parent = row.kind == DownloadHierarchyRowKind::Series ||
                            row.kind == DownloadHierarchyRowKind::Season;
        const char* kindLabel = row.kind == DownloadHierarchyRowKind::Movie    ? "MOVIE"
                                : row.kind == DownloadHierarchyRowKind::Series ? "SERIES"
                                : row.kind == DownloadHierarchyRowKind::Season ? "SEASON"
                                                                               : "EP";
        int tx = rx + 12;
        tx += ui::chip(fb, tx, ry + 8, kindLabel, d::kAccentSoft, d::kAccentHi) + 8;

        // State chip on the right.
        std::string stateText;
        d::Rgb stateBg = d::kRaised, stateFg = d::kTextSecondary;
        int percent = 0;
        if (item) {
            percent = static_cast<int>(downloadPercent(*item));
            stateText = downloadStateLabel(item->state);
            switch (item->state) {
            case DownloadState::Complete:
                stateBg = ui::mix(d::kCanvas, d::kSuccess, 22);
                stateFg = d::kSuccess;
                break;
            case DownloadState::Downloading:
                stateBg = d::kAccentSoft;
                stateFg = d::kAccentHi;
                stateText += " " + std::to_string(percent) + "%";
                break;
            case DownloadState::Failed:
            case DownloadState::Unauthorized:
                stateBg = ui::mix(d::kCanvas, d::kDanger, 24);
                stateFg = d::kDanger;
                break;
            case DownloadState::UpdateAvailable:
            case DownloadState::WaitingForNetwork:
                stateBg = ui::mix(d::kCanvas, d::kWarning, 22);
                stateFg = d::kWarning;
                break;
            default:
                break;
            }
        } else {
            percent = row.aggregate.progressKnown ? row.aggregate.progress : 0;
            stateText = std::to_string(row.aggregate.episodes) +
                        (row.aggregate.episodes == 1 ? " episode" : " episodes");
            if (row.aggregate.active)
                stateText += " " + std::to_string(row.aggregate.progress) + "%";
        }
        const int chipW = ui::textWidth(stateText) + 12;
        ui::chip(fb, rx + rw - chipW - 12, ry + 8, stateText, stateBg, stateFg);

        std::string title = row.title;
        if (parent)
            title += row.expanded ? "  v" : "  >";
        ui::textClamped(fb, tx, ry + 10, rx + rw - chipW - 24 - tx, title,
                        selected ? d::kText : d::kTextSecondary);

        // Detail line.
        std::string detail;
        if (!item) {
            if (row.aggregate.complete)
                detail = std::to_string(row.aggregate.complete) + " complete";
            if (row.aggregate.bytesKnown) {
                if (!detail.empty())
                    detail += "  ";
                detail += formatBytes(row.aggregate.bytes) + " / " +
                          formatBytes(row.aggregate.totalBytes);
            }
        } else {
            const bool completedHls = item->hlsStorage && downloadHierarchyComplete(*item);
            const std::string sizeLabel = (item->hlsStorage && !completedHls ? "~" : "") +
                                          formatBytes(displayDownloadBytes(*item));
            if (item->itemType == "episode")
                detail = episodeDownloadLabel(*item) + "  " + sizeLabel;
            else
                detail = sizeLabel;
            if (item->state == DownloadState::Downloading) {
                char active[96];
                std::snprintf(active, sizeof(active), "%s  %s/s", sizeLabel.c_str(),
                              formatBytes(item->recentBytesPerSec).c_str());
                detail = active;
            }
            if (!item->lastError.empty() && (item->state == DownloadState::Failed ||
                                             item->state == DownloadState::WaitingForNetwork ||
                                             item->state == DownloadState::Unauthorized))
                detail = item->lastError;
        }
        ui::textClamped(fb, rx + 12, ry + 30, rw - 24, detail, d::kTextMuted);
        const bool done = item && item->state == DownloadState::Complete;
        ui::progressBar(fb, rx + 12, ry + DL_ROW_H - 10, rw - 24, 4, percent,
                        done ? d::kSuccess : d::kAccent);
    }
}

void HomeScreen::drawBottomHints(SDL_Surface* fb)
{
    ui::FooterSpec spec;
    using ui::Key;
    std::string link;
    linkPresentation(m_userName, m_link ? m_link->status() : LinkStatus::Checking, link, spec.link);
    spec.rightText = link;
    spec.note = syncStatusText();

    if (m_loadState == LoadState::Loading) {
        spec.hints = {{Key::Y, "Log out"}};
    } else if (m_loadState == LoadState::Error) {
        spec.hints = {{Key::A, "Retry"}, {Key::Y, "Log out"}};
    } else if (m_logoutArmed && !m_logoutRequested) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)",
                      static_cast<int>((m_logoutTimer + 999) / 1000));
        spec.message = buf;
        spec.messageColor = d::kDanger;
    } else if (activeTabNamed("Settings")) {
        if (m_settingsState.confirmation == SettingsConfirmation::ChangeServer) {
            spec.message = "Press A again to change server";
            spec.messageColor = d::kWarning;
        } else if (m_settingsState.confirmation == SettingsConfirmation::Logout) {
            spec.message = "Press A again to log out";
            spec.messageColor = d::kDanger;
        } else {
            spec.hints = {
                {Key::Dpad, "Scroll"}, {Key::A, "Change"}, {Key::B, "Back"}, {Key::LR, "Tabs"}};
        }
    } else if (activeTabNamed("Downloads")) {
        const DownloadHierarchyRow* selectedRow =
            m_downloadsState.selected >= 0 &&
                    m_downloadsState.selected <
                        static_cast<int>(m_downloadsState.hierarchy.visible.size())
                ? &m_downloadsState.hierarchy.visible[m_downloadsState.selected]
                : nullptr;
        const DownloadItem* selectedItem = selectedRow ? selectedRow->item : nullptr;
        if (!m_downloadsState.journalDiscardConfirmId.empty()) {
            spec.message = "Press X again to discard missing progress";
            spec.messageColor = d::kWarning;
        } else if (!m_downloadsState.confirmId.empty() && selectedRow &&
                   m_downloadsState.confirmId == selectedRow->id) {
            char confirm[96];
            if (selectedItem) {
                std::snprintf(confirm, sizeof(confirm), "Press Y again to %s %s",
                              downloadRemoveIsDelete(*selectedItem) ? "delete" : "cancel",
                              formatBytes(displayDownloadBytes(*selectedItem)).c_str());
            } else {
                std::snprintf(confirm, sizeof(confirm), "Press Y again to delete %s (%u local)",
                              selectedRow->kind == DownloadHierarchyRowKind::Season
                                  ? "Season"
                                  : "entire Series",
                              static_cast<unsigned>(m_downloadsState.confirmItemIds.size()));
            }
            spec.message = confirm;
            spec.messageColor = d::kDanger;
        } else {
            std::string primary = "Select";
            if (selectedItem)
                primary = downloadPrimaryControlLabel(downloadPrimaryControl(selectedItem->state));
            if (selectedRow && !selectedItem)
                primary = selectedRow->expanded ? "Collapse" : "Expand";
            spec.hints = {{Key::Dpad, "Move"}, {Key::A, primary}};
            if (selectedItem && selectedItem->state == DownloadState::UpdateAvailable)
                spec.hints.push_back({Key::X, "Update"});
            else if (!m_downloadsState.missingJournal.empty())
                spec.hints.push_back({Key::X, "Discard"});
            spec.hints.push_back({Key::Y, selectedRow && !selectedItem ? "Delete all" : "Delete"});
            spec.hints.push_back({Key::B, "Back"});
        }
    } else if (activeTabNamed("Movies")) {
        spec.hints =
            m_movieRailFocused
                ? std::vector<ui::Hint>{{Key::Dpad, "Move"},
                                        {Key::A, "Filter"},
                                        {Key::B, "Back"},
                                        {Key::LR, "Tabs"}}
                : std::vector<ui::Hint>{
                      {Key::Dpad, "Browse"}, {Key::A, "Open"}, {Key::B, "Back"}, {Key::LR, "Tabs"}};
    } else if (activeTabNamed("Shows")) {
        spec.hints =
            m_showsFocus == ShowsFocus::AlphabetRail
                ? std::vector<ui::Hint>{{Key::Dpad, "Move"},
                                        {Key::A, "Filter"},
                                        {Key::B, "Back"},
                                        {Key::LR, "Tabs"}}
                : std::vector<ui::Hint>{
                      {Key::Dpad, "Browse"}, {Key::A, "Open"}, {Key::B, "Back"}, {Key::LR, "Tabs"}};
    } else {
        spec.hints = {
            {Key::Dpad, "Navigate"}, {Key::A, "Select"}, {Key::B, "Back"}, {Key::LR, "Tabs"}};
    }
    ui::footer(fb, spec);
}

void HomeScreen::drawLoadingState(SDL_Surface* fb)
{
    const int phase = static_cast<int>((SDL_GetTicks() / 350) % 4);
    std::string title = "Loading your library";
    title.append(static_cast<std::size_t>(phase), '.');
    // Keep the title from jumping as the dots animate: pad to a fixed width.
    const std::string padded = title + std::string(static_cast<std::size_t>(3 - phase), ' ');
    drawCenteredNote(fb, padded, m_userName.empty() ? std::string() : "Signed in as " + m_userName,
                     d::kText);
}

void HomeScreen::drawErrorState(SDL_Surface* fb)
{
    drawCenteredNote(fb, "Could not load your library", m_fetchError, d::kDanger);
}

} // namespace miyoofin
