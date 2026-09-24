#include "HomeScreen.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../ArtworkPresentation.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
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

// Layout constants
static constexpr int TAB_Y = 0;
static constexpr int TAB_H = 24;    // compact header used by non-Home tabs
static constexpr int HEADER_H = 64; // Home-only large wordmark + status cluster
static constexpr int BOTTOM_H = 24;
static constexpr int ROWS_Y = 70; // first Home rail heading, below the header
static constexpr int ROW_LABEL_H = 16;
static constexpr int ROW_LABEL_GAP = 4;
static constexpr int ROW_CAPTION_H = 34; // per-card title + subtitle lines
static constexpr int ROW_STRIP_H = HOME_RAIL_STRIP_H;
static constexpr int ROW_PITCH = 194; // two rails land above the bottom bar
static constexpr int VISIBLE_ROWS = 2;
static constexpr int POSTER_MAX_CONCURRENT = 4;
static constexpr size_t POSTER_MAX_BYTES = 256 * 1024;
static constexpr int SEASON_POSTER_W = 74;
static constexpr int SEASON_POSTER_H = 111;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;
static constexpr int SHOWS_RAIL_W = 36, SHOWS_PREVIEW_H = 105, SHOWS_GRID_TOP = 153;
static constexpr int SHOWS_HALF_W = 302, SHOWS_LEFT_X = 36, SHOWS_RIGHT_X = 338;
static constexpr std::int64_t SYNC_FRESH_WALL_MS = 15LL * 60 * 1000;
static constexpr std::int64_t HIERARCHY_RECONCILE_MS = 24LL * 60 * 60 * 1000;

// Phase 2 Home presentation style.  Near-black navy canvas, electric-blue
// focus glow, blue accent pill and black artwork placeholders.  Kept
// presentation-local so no other screen or domain model is affected.
static constexpr Uint8 HOME_BG_R = 7, HOME_BG_G = 9, HOME_BG_B = 18;
static constexpr Uint8 HEADER_BG_R = 4, HEADER_BG_G = 6, HEADER_BG_B = 14;
static constexpr Uint8 BOTTOM_BG_R = 4, BOTTOM_BG_G = 5, BOTTOM_BG_B = 10;
static constexpr Uint8 FOCUS_R = 60, FOCUS_G = 150, FOCUS_B = 255;
static constexpr Uint8 FOCUS_GLOW_R = 24, FOCUS_GLOW_G = 70, FOCUS_GLOW_B = 150;
static constexpr Uint8 CARD_BORDER_R = 70, CARD_BORDER_G = 76, CARD_BORDER_B = 92;
static constexpr Uint8 MUTED_TEXT_R = 175, MUTED_TEXT_G = 180, MUTED_TEXT_B = 195;
static constexpr int HEADER_BRAND_X = 16; // compact (non-Home) brand mark

// Two-tone wordmark geometry.
static constexpr int LOGO_X = 12;
static constexpr int LOGO_Y = 8;
static constexpr int LOGO_SCALE = 2;

/// Rounded-rectangle fill built from rects only; the uncovered corner
/// squares read as rounded corners against the darker header.
static void fillRoundedRect(SDL_Surface* fb, int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b,
                            int radius)
{
    if (radius <= 0 || w <= 2 * radius || h <= 2 * radius) {
        BitmapFont::fillRect(fb, x, y, w, h, r, g, b, 255);
        return;
    }
    BitmapFont::fillRect(fb, x + radius, y, w - 2 * radius, h, r, g, b, 255);
    BitmapFont::fillRect(fb, x, y + radius, w, h - 2 * radius, r, g, b, 255);
}

static void drawFocusGlow(SDL_Surface* fb, int x, int y, int w, int h)
{
    // Layered frames approximate a soft electric-blue glow using solid rects.
    BitmapFont::drawRect(fb, x - 4, y - 4, w + 8, h + 8, FOCUS_GLOW_R, FOCUS_GLOW_G, FOCUS_GLOW_B);
    BitmapFont::drawRect(fb, x - 3, y - 3, w + 6, h + 6, 34, 95, 180);
    BitmapFont::drawRect(fb, x - 2, y - 2, w + 4, h + 4, FOCUS_R, FOCUS_G, FOCUS_B);
    BitmapFont::drawRect(fb, x - 1, y - 1, w + 2, h + 2, 90, 175, 255);
    BitmapFont::drawRect(fb, x, y, w, h, 170, 220, 255);
}

/// Continue Watching progress bar drawn inside the lower edge of a card.
static void drawContinueWatchingProgress(SDL_Surface* fb, int x, int y, int w, int h,
                                         const MediaItem& item)
{
    constexpr int BAR_H = 4;
    constexpr int INSET = 6;
    const int bx = x + INSET;
    const int bw = std::max(1, w - 2 * INSET);
    const int by = y + h - INSET - BAR_H;
    const bool hasProgress = item.played || item.progress > 0.0f || item.playbackPositionTicks > 0;
    BitmapFont::fillRect(fb, bx, by, bw, BAR_H, 52, 58, 72, 255);
    if (!hasProgress)
        return;
    const int pct = item.played ? 100 : playbackPercent(item);
    const int fillW = bw * std::max(0, std::min(100, pct)) / 100;
    if (fillW > 0)
        BitmapFont::fillRect(fb, bx, by, fillW, BAR_H, FOCUS_R, FOCUS_G, FOCUS_B, 255);
}

/// Per-card title (white) and subtitle (light blue) beneath a Home rail card.
/// For episodes the series name is the title and the episode title is the
/// subtitle; otherwise the item title is followed by its year.
static void drawCardCaption(SDL_Surface* fb, int x, int y, int w, const MediaItem& item,
                            bool focused)
{
    std::string title = item.title;
    std::string subtitle;
    if (item.type == "episode" && !item.seriesName.empty()) {
        title = item.seriesName;
        subtitle = item.title;
    } else if (item.year > 0) {
        subtitle = std::to_string(item.year);
    }
    const int maxGlyphs = std::max(1, (w - 2) / BitmapFont::GLYPH_W);
    const std::string shownTitle = BitmapFont::truncateUtf8(title, maxGlyphs);
    BitmapFont::drawString(fb, x, y, shownTitle.c_str(), focused ? 255 : 228, focused ? 255 : 232,
                           focused ? 255 : 242, HOME_BG_R, HOME_BG_G, HOME_BG_B);
    if (!subtitle.empty()) {
        const std::string shownSub = BitmapFont::truncateUtf8(subtitle, maxGlyphs);
        BitmapFont::drawString(fb, x, y + BitmapFont::GLYPH_H, shownSub.c_str(), 130, 180, 240,
                               HOME_BG_R, HOME_BG_G, HOME_BG_B);
    }
}

// --- Header status icons (rects/text only) --------------------------------

static void drawWifiIcon(SDL_Surface* fb, int x, int y)
{
    BitmapFont::fillRect(fb, x, y + 9, 16, 2, 195, 205, 222, 255);
    BitmapFont::fillRect(fb, x + 2, y + 5, 12, 2, 195, 205, 222, 255);
    BitmapFont::fillRect(fb, x + 4, y + 1, 8, 2, 195, 205, 222, 255);
    BitmapFont::fillRect(fb, x + 7, y + 9, 3, 3, 90, 205, 120, 255);
}

static void drawBatteryIcon(SDL_Surface* fb, int x, int y)
{
    BitmapFont::drawRect(fb, x, y, 22, 11, 190, 200, 215);
    BitmapFont::fillRect(fb, x + 2, y + 2, 13, 7, 110, 210, 120, 255);
    BitmapFont::fillRect(fb, x + 22, y + 3, 2, 5, 190, 200, 215, 255);
}

static void drawGearIcon(SDL_Surface* fb, int x, int y)
{
    const Uint8 r = 190, g = 198, b = 215;
    BitmapFont::fillRect(fb, x + 2, y - 2, 4, 4, r, g, b, 255);
    BitmapFont::fillRect(fb, x + 2, y + 12, 4, 4, r, g, b, 255);
    BitmapFont::fillRect(fb, x - 2, y + 2, 4, 4, r, g, b, 255);
    BitmapFont::fillRect(fb, x + 12, y + 2, 4, 4, r, g, b, 255);
    BitmapFont::fillRect(fb, x, y + 2, 14, 10, r, g, b, 255);
    BitmapFont::fillRect(fb, x + 2, y, 10, 14, r, g, b, 255);
    BitmapFont::fillRect(fb, x + 5, y + 5, 4, 4, HEADER_BG_R, HEADER_BG_G, HEADER_BG_B, 255);
}

// --- Bottom status bar ----------------------------------------------------

static void drawDpadIcon(SDL_Surface* fb, int x, int y)
{
    const Uint8 r = 150, g = 156, b = 172;
    BitmapFont::fillRect(fb, x + 6, y, 6, 18, r, g, b, 255);
    BitmapFont::fillRect(fb, x, y + 6, 18, 6, r, g, b, 255);
}

static void drawButtonBadge(SDL_Surface* fb, int x, int y, char ch, Uint8 r, Uint8 g, Uint8 b)
{
    fillRoundedRect(fb, x, y, 14, 14, r, g, b, 3);
    char s[2] = {ch, '\0'};
    BitmapFont::drawString(fb, x + 3, y - 1, s, 255, 255, 255, r, g, b);
}

static void drawHomeStatusBar(SDL_Surface* fb, int y)
{
    BitmapFont::fillRect(fb, 0, y, 640, BOTTOM_H, BOTTOM_BG_R, BOTTOM_BG_G, BOTTOM_BG_B, 255);
    BitmapFont::fillRect(fb, 0, y, 640, 1, 34, 60, 100, 255);
    const int textY = y + 4;
    drawDpadIcon(fb, 10, y + 3);
    BitmapFont::drawString(fb, 32, textY, "Navigate", 200, 205, 215, BOTTOM_BG_R, BOTTOM_BG_G,
                           BOTTOM_BG_B);
    drawButtonBadge(fb, 110, y + 5, 'A', 45, 120, 235);
    BitmapFont::drawString(fb, 128, textY, "Select", 200, 205, 215, BOTTOM_BG_R, BOTTOM_BG_G,
                           BOTTOM_BG_B);
    drawButtonBadge(fb, 196, y + 5, 'B', 205, 70, 80);
    BitmapFont::drawString(fb, 214, textY, "Back", 200, 205, 215, BOTTOM_BG_R, BOTTOM_BG_G,
                           BOTTOM_BG_B);

    const char* status = "Jellyfin Connected";
    const int sx = 640 - 12 - (int)::strlen(status) * BitmapFont::GLYPH_W;
    BitmapFont::fillRect(fb, sx - 12, y + 8, 7, 7, 60, 200, 110, 255);
    BitmapFont::drawString(fb, sx, textY, status, 200, 210, 220, BOTTOM_BG_R, BOTTOM_BG_G,
                           BOTTOM_BG_B);
}

static void blitDecoded(SDL_Surface* fb, const DecodedImage& img, int x, int y, int w, int h)
{
    if (img.empty())
        return;
    const float ia = (float)img.width / img.height;
    const float ba = (float)w / h;
    const int dw = ia > ba ? w : (int)(h * ia + .5f);
    const int dh = ia > ba ? (int)(w / ia + .5f) : h;
    SDL_Surface* s =
        SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(), img.width, img.height, 32, img.width * 4,
                                 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    if (s) {
        SDL_Rect src = {0, 0, img.width, img.height};
        SDL_Rect dst = {x + (w - dw) / 2, y + (h - dh) / 2, dw, dh};
        SDL_BlitScaled(s, &src, fb, &dst);
        SDL_FreeSurface(s);
    }
}

void HomeScreen::render(SDL_Surface* fb)
{
    if (m_loadState == LoadState::Ready && !m_firstInteractiveFrameLogged) {
        m_firstInteractiveFrameLogged = true;
        uiDiagnostics().log("[HomeScreen] startup stage=first_interactive_frame");
    }
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
            BitmapFont::fillRect(fb, 0, HEADER_H, 640, 480 - HEADER_H - BOTTOM_H, HOME_BG_R,
                                 HOME_BG_G, HOME_BG_B, 255);
            drawRowList(fb);
        }
    }
    drawBottomHints(fb);
}
void HomeScreen::drawTabBar(SDL_Surface* fb)
{
    // The tall branded header belongs to Home only.  Movies, Shows,
    // Downloads and Settings keep the legacy 24px compact header so their
    // existing y=25 content geometry is not overlapped.
    const bool home = activeTabNamed("Home");
    const int headerH = home ? HEADER_H : TAB_H;
    BitmapFont::fillRect(fb, 0, TAB_Y, 640, headerH, HEADER_BG_R, HEADER_BG_G, HEADER_BG_B, 255);

    int tabTextY = TAB_Y + (headerH - BitmapFont::GLYPH_H) / 2;
    if (home) {
        // Large two-tone wordmark with the tagline beneath it.
        BitmapFont::drawStringScaled(fb, LOGO_X, LOGO_Y, "Miyoo", LOGO_SCALE, 238, 244, 255,
                                     HEADER_BG_R, HEADER_BG_G, HEADER_BG_B);
        BitmapFont::drawStringScaled(fb, LOGO_X + 5 * BitmapFont::GLYPH_W * LOGO_SCALE, LOGO_Y,
                                     "Fin", LOGO_SCALE, 70, 150, 255, HEADER_BG_R, HEADER_BG_G,
                                     HEADER_BG_B);
        BitmapFont::drawString(fb, LOGO_X + 1, LOGO_Y + LOGO_SCALE * BitmapFont::GLYPH_H + 2,
                               "Your Media. Anywhere.", 130, 175, 232, HEADER_BG_R, HEADER_BG_G,
                               HEADER_BG_B);
        tabTextY = 16;
    } else {
        // Compact brand mark for the non-Home tabs.
        BitmapFont::drawString(fb, HEADER_BRAND_X, tabTextY, "MiyooFin", FOCUS_R, FOCUS_G, FOCUS_B,
                               HEADER_BG_R, HEADER_BG_G, HEADER_BG_B);
    }

    // Compact nav centered across the header; the active tab sits in a
    // bright blue rounded pill.  Shared by every tab so the centered
    // headerTabsStartX() hit regions stay valid.
    int x = headerTabsStartX(m_tabs);
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        const std::string& name = m_tabs[i].name;
        const int textW = (int)name.size() * BitmapFont::GLYPH_W;
        if (i == m_activeTab) {
            fillRoundedRect(fb, x - 6, tabTextY - 3, textW + 12, BitmapFont::GLYPH_H + 6, 42, 118,
                            232, 4);
            BitmapFont::drawString(fb, x, tabTextY, name.c_str(), 255, 255, 255, 42, 118, 232);
        } else {
            BitmapFont::drawString(fb, x, tabTextY, name.c_str(), MUTED_TEXT_R, MUTED_TEXT_G,
                                   MUTED_TEXT_B, HEADER_BG_R, HEADER_BG_G, HEADER_BG_B);
        }
        x += textW + 16;
    }

    if (home) {
        // Status cluster (Wi-Fi, battery, clock, gear) pinned to the top right.
        drawWifiIcon(fb, 514, 22);
        drawBatteryIcon(fb, 540, 24);
        std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_WIN32)
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char clock[8];
        std::snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        BitmapFont::drawString(fb, 570, tabTextY, clock, 235, 240, 255, HEADER_BG_R, HEADER_BG_G,
                               HEADER_BG_B);
        drawGearIcon(fb, 618, 21);
    } else {
        // Legacy right-aligned sync status and login label in the compact bar.
        std::string status = syncStatusText();
        std::string login = "Logged in as: " + m_userName;
        const int maxChars = 24;
        if ((int)login.size() > maxChars)
            login = login.substr(0, maxChars - 3) + "...";
        int loginX = 640 - 8 - (int)login.size() * BitmapFont::GLYPH_W;
        if (loginX > x + 4)
            BitmapFont::drawString(fb, loginX, tabTextY, login.c_str(), Theme::TEXT_R,
                                   Theme::TEXT_G, Theme::TEXT_B, HEADER_BG_R, HEADER_BG_G,
                                   HEADER_BG_B);
        int statusX = loginX - 8 - (int)status.size() * BitmapFont::GLYPH_W;
        if (!status.empty() && statusX > x + 4)
            BitmapFont::drawString(fb, statusX, tabTextY, status.c_str(), FOCUS_R, FOCUS_G, FOCUS_B,
                                   HEADER_BG_R, HEADER_BG_G, HEADER_BG_B);
    }

    BitmapFont::fillRect(fb, 0, headerH - 1, 640, 1, FOCUS_GLOW_R, FOCUS_GLOW_G, FOCUS_GLOW_B, 255);
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
    for (int ri = 0; ri < VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size())
            break;
        const MediaRow& row = rows[rowIdx];
        const bool rowFocused = rowIdx == m_activeRow;
        const bool continueWatching = row.label == "Continue Watching";
        const int rowY = ROWS_Y + ri * ROW_PITCH;
        const int stripTop = rowY + ROW_LABEL_H + ROW_LABEL_GAP;
        const int captionY = stripTop + ROW_STRIP_H + ROW_LABEL_GAP;

        // Rail heading and right-aligned "View All" affordance.
        BitmapFont::drawString(fb, HOME_RAIL_MARGIN, rowY, row.label.c_str(),
                               rowFocused ? FOCUS_R : 235, rowFocused ? FOCUS_G : 240,
                               rowFocused ? FOCUS_B : 255, HOME_BG_R, HOME_BG_G, HOME_BG_B);
        static const char* kViewAll = "View All >";
        const int viewAllX = 640 - HOME_RAIL_MARGIN - (int)::strlen(kViewAll) * BitmapFont::GLYPH_W;
        BitmapFont::drawString(fb, viewAllX, rowY, kViewAll, 120, 175, 240, HOME_BG_R, HOME_BG_G,
                               HOME_BG_B);

        // Cards with per-item sizing and pixel-scroll offset.
        int cardAccumX = HOME_RAIL_MARGIN;
        for (int ci = 0; ci < (int)row.items.size(); ++ci) {
            const MediaItem& item = row.items[ci];
            ArtworkBox sz = homeRailCardSize(item);
            int screenX = cardAccumX - rowCardScrollOffset(rowIdx, m_activeRow, m_cardScroll);
            if (screenX + sz.w < HOME_RAIL_MARGIN) {
                // Fully off-screen left
                cardAccumX += sz.w + HOME_RAIL_GAP;
                continue;
            }
            if (screenX > 640 - HOME_RAIL_MARGIN)
                break;
            const bool sel = rowFocused && ci == m_activeCard;
            int cardScreenY = stripTop + (ROW_STRIP_H - sz.h) / 2;
            drawCard(fb, screenX, cardScreenY, sz.w, sz.h, item, sel, CardPresentation::Home);
            if (continueWatching)
                drawContinueWatchingProgress(fb, screenX, cardScreenY, sz.w, sz.h, item);
            drawCardCaption(fb, screenX, captionY, sz.w, item, sel);
            cardAccumX += sz.w + HOME_RAIL_GAP;
        }
    }
}

void HomeScreen::drawMovieGrid(SDL_Surface* fb)
{
    const MediaRow* row = currentRow();
    if (!row)
        return;
    const int top = 134, gap = 6;
    for (int i = 0; i < (int)row->items.size(); ++i) {
        int gr = i / MOVIE_GRID_COLUMNS, gc = i % MOVIE_GRID_COLUMNS;
        if (gr < m_rowScroll || gr >= m_rowScroll + MOVIE_GRID_ROWS)
            continue;
        int x = 42 + gc * (64 + gap), y = top + (gr - m_rowScroll) * (96 + gap);
        drawCard(fb, x, y, 64, 96, row->items[i], !m_movieRailFocused && i == m_activeCard);
    }
    if (row->items.empty() && m_movieActiveLetter >= 0) {
        char message[48];
        std::snprintf(message, sizeof(message), "No movies starting with %c",
                      'A' + m_movieActiveLetter);
        BitmapFont::drawString(fb, 48, 210, message, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }
}

void HomeScreen::drawMovieAlphabetRail(SDL_Surface* fb)
{
    BitmapFont::fillRect(fb, 0, 25, 36, 437, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 35, 25, 1, 437, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    for (int i = 0; i < 26; ++i) {
        int y = 27 + i * 16;
        bool focused = m_movieRailFocused && i == m_movieAlphabetFocus;
        bool active = i == m_movieActiveLetter;
        if (focused)
            BitmapFont::fillRect(fb, 2, y - 1, 31, BitmapFont::GLYPH_H + 2, Theme::ACCENT_R,
                                 Theme::ACCENT_G, Theme::ACCENT_B, 120);
        char letter[2] = {static_cast<char>('A' + i), '\0'};
        BitmapFont::drawString(fb, 14, y, letter,
                               active    ? Theme::HIGHLIGHT_R
                               : focused ? Theme::BG_R
                                         : Theme::TEXT_R,
                               active    ? Theme::HIGHLIGHT_G
                               : focused ? Theme::BG_G
                                         : Theme::TEXT_G,
                               active    ? Theme::HIGHLIGHT_B
                               : focused ? Theme::BG_B
                                         : Theme::TEXT_B,
                               focused ? Theme::ACCENT_R : 24, focused ? Theme::ACCENT_G : 24,
                               focused ? Theme::ACCENT_B : 32);
        if (active && !focused)
            BitmapFont::fillRect(fb, 4, y + BitmapFont::GLYPH_H + 1, 27, 1, Theme::HIGHLIGHT_R,
                                 Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B, 255);
    }
}

void HomeScreen::drawMoviePreview(SDL_Surface* fb)
{
    BitmapFont::fillRect(fb, 36, 25, 604, 105, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 36, 129, 604, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                         70);
    const MediaItem* item = currentItem();
    if (!item)
        return;
    int px = 42, py = 29;
    const SDL_Color color = presentationArtworkColor(*item);
    BitmapFont::fillRect(fb, px, py, 64, 96, color.r, color.g, color.b, color.a);
    if (!m_selectedArtwork.empty()) {
        char cacheKey[512];
        std::snprintf(cacheKey, sizeof(cacheKey), "%s:%dx%d", m_selectedArtworkId.c_str(), 64, 96);
        std::string ck(cacheKey);
        auto cached = m_cardSurfaceCache.find(ck);
        if (cached != m_cardSurfaceCache.end() && cached->second) {
            SDL_Surface* cs = cached->second;
            SDL_Rect dst = {px + (64 - cs->w) / 2, py + (96 - cs->h) / 2, cs->w, cs->h};
            SDL_BlitSurface(cs, nullptr, fb, &dst);
        } else {
            SDL_Surface* image = SDL_CreateRGBSurfaceFrom(
                (void*)m_selectedArtwork.pixels.data(), m_selectedArtwork.width,
                m_selectedArtwork.height, 32, m_selectedArtwork.width * 4, 0x000000FF, 0x0000FF00,
                0x00FF0000, 0xFF000000);
            if (image) {
                SDL_Rect src = {0, 0, m_selectedArtwork.width, m_selectedArtwork.height},
                         dst = {px, py, 64, 96};
                SDL_BlitScaled(image, &src, fb, &dst);
                // Do NOT cache a surface wrapping m_selectedArtwork — it is
                // transient and may be freed before the cached surface is
                // evicted, causing a use-after-free.
                SDL_FreeSurface(image);
            }
        }
    }
    BitmapFont::drawRect(fb, px, py, 64, 96, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
    int x = 114, y = 33;
    std::string truncatedTitle = BitmapFont::truncateUtf8(item->title, 65);
    BitmapFont::drawString(fb, x, y, truncatedTitle.c_str(), Theme::ACCENT_R, Theme::ACCENT_G,
                           Theme::ACCENT_B, 24, 24, 32);
    char meta[96] = {};
    int n = 0;
    if (item->year > 0)
        n += std::snprintf(meta + n, sizeof(meta) - n, "%d", item->year);
    int mins = ticksToMinutes(item->runTimeTicks);
    if (mins > 0)
        n += std::snprintf(meta + n, sizeof(meta) - n, "%s%dh %dm", n ? " * " : "", mins / 60,
                           mins % 60);
    if (item->rating > 0)
        std::snprintf(meta + n, sizeof(meta) - n, "%s%.1f", n ? " * " : "", (double)item->rating);
    BitmapFont::drawString(fb, x, y + 18, meta, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 24, 24,
                           32);
    char state[96];
    std::snprintf(state, sizeof(state), "%s%s", item->genre.c_str(),
                  item->played         ? (item->genre.empty() ? "Watched" : " * Watched")
                  : item->progress > 0 ? ""
                                       : "");
    if (item->progress > 0 && !item->played)
        std::snprintf(state, sizeof(state), "%s%s%d%% watched", item->genre.c_str(),
                      item->genre.empty() ? "" : " * ", (int)(item->progress * 100.0f + 0.5f));
    BitmapFont::drawString(fb, x, y + 36, state, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 24,
                           24, 32);
}

void HomeScreen::drawShowsAlphabetRail(SDL_Surface* fb)
{
    BitmapFont::fillRect(fb, 0, 25, SHOWS_RAIL_W, 437, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 35, 25, 1, 437, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    for (int i = 0; i < 26; ++i) {
        int y = 27 + i * 16;
        bool focused = m_showsFocus == ShowsFocus::AlphabetRail && i == m_showsAlphabetFocus;
        bool active = i == m_showsActiveLetter;
        if (focused)
            BitmapFont::fillRect(fb, 2, y - 1, 31, BitmapFont::GLYPH_H + 2, Theme::ACCENT_R,
                                 Theme::ACCENT_G, Theme::ACCENT_B, 120);
        char c[2] = {char('A' + i), 0};
        BitmapFont::drawString(fb, 14, y, c,
                               active    ? Theme::HIGHLIGHT_R
                               : focused ? Theme::BG_R
                                         : Theme::TEXT_R,
                               active    ? Theme::HIGHLIGHT_G
                               : focused ? Theme::BG_G
                                         : Theme::TEXT_G,
                               active    ? Theme::HIGHLIGHT_B
                               : focused ? Theme::BG_B
                                         : Theme::TEXT_B,
                               focused ? Theme::ACCENT_R : 24, focused ? Theme::ACCENT_G : 24,
                               focused ? Theme::ACCENT_B : 32);
    }
}

void HomeScreen::drawShowsPreview(SDL_Surface* fb)
{
    BitmapFont::fillRect(fb, 36, 25, 604, SHOWS_PREVIEW_H, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 36, 129, 604, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                         70);
    const MediaItem* item = showsSelectedItem();
    if (!item)
        return;
    int px = 42;
    int py = 29;
    const SDL_Color tint = presentationArtworkColor(*item);
    BitmapFont::fillRect(fb, px, py, 64, 96, tint.r, tint.g, tint.b, tint.a);
    std::string key = rowArtworkKey(*item);
    auto it = m_rowArtwork.find(key);
    const DecodedImage* imgPtr = nullptr;
    bool fromRowArtwork = false;
    if (it != m_rowArtwork.end() && it->second.status == RowArtworkStatus::Loaded &&
        it->second.image) {
        imgPtr = it->second.image.get();
        fromRowArtwork = true;
    } else if (!m_selectedArtwork.empty()) {
        imgPtr = &m_selectedArtwork;
    }
    if (imgPtr && !imgPtr->empty()) {
        char ckBuf[512];
        std::snprintf(ckBuf, sizeof(ckBuf), "%s:64x96",
                      key.empty() ? m_selectedArtworkId.c_str() : key.c_str());
        std::string ck(ckBuf);
        auto cached = m_cardSurfaceCache.find(ck);
        if (cached != m_cardSurfaceCache.end() && cached->second) {
            SDL_Surface* cs = cached->second;
            SDL_Rect dst = {px + (64 - cs->w) / 2, py + (96 - cs->h) / 2, cs->w, cs->h};
            SDL_BlitSurface(cs, nullptr, fb, &dst);
        } else {
            blitDecoded(fb, *imgPtr, px, py, 64, 96);
            if (!key.empty() && fromRowArtwork)
                prepareCardSurface(ck, *imgPtr, 64, 96);
        }
    }
    BitmapFont::drawRect(fb, px, py, 64, 96, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
    BitmapFont::drawString(fb, 114, 33, item->title.c_str(), Theme::ACCENT_R, Theme::ACCENT_G,
                           Theme::ACCENT_B, 24, 24, 32);
    char meta[96] = {};
    int n = 0;
    if (item->year)
        n += std::snprintf(meta + n, sizeof(meta) - n, "%d", item->year);
    if (item->rating > 0)
        std::snprintf(meta + n, sizeof(meta) - n, "%s%.1f", n ? " * " : "", (double)item->rating);
    BitmapFont::drawString(fb, 114, 51, meta, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 24, 24,
                           32);
    char state[96];
    std::snprintf(state, sizeof(state), "%s%s", item->genre.c_str(),
                  item->played         ? " * Watched"
                  : item->progress > 0 ? " * In progress"
                                       : "");
    BitmapFont::drawString(fb, 114, 69, state, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, 24, 24,
                           32);
}

void HomeScreen::drawShowsGrid(SDL_Surface* fb)
{
    BitmapFont::drawString(fb, 44, 137, "SHOWS", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::drawString(fb, 346, 137, "ANIME", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::fillRect(fb, 337, 135, 1, 327, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                         100);
    auto draw = [&](const std::vector<MediaItem>& items, int scroll, int selected, bool focused,
                    int base) {
        for (int i = 0; i < (int)items.size(); ++i) {
            int row = i / 4;
            if (row < scroll || row >= scroll + 3)
                continue;
            drawCard(fb, base + 14 + (i % 4) * 70, SHOWS_GRID_TOP + (row - scroll) * 102, 64, 96,
                     items[i], focused && i == selected);
        }
    };
    draw(m_filteredShows, m_showScroll, m_showSelected, m_showsFocus == ShowsFocus::ShowsGrid,
         SHOWS_LEFT_X);
    draw(m_filteredAnime, m_animeScroll, m_animeSelected, m_showsFocus == ShowsFocus::AnimeGrid,
         SHOWS_RIGHT_X);
    if (m_filteredShows.empty() && m_filteredAnime.empty()) {
        char b[64];
        if (m_showsActiveLetter >= 0)
            std::snprintf(b, sizeof(b), "No shows or anime starting with %c",
                          'A' + m_showsActiveLetter);
        else
            std::snprintf(b, sizeof(b), "No shows on this server");
        BitmapFont::drawString(fb, 48, 230, b, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }
}

void HomeScreen::drawCard(SDL_Surface* fb, int x, int y, int w, int h, const MediaItem& item,
                          bool selected, CardPresentation presentation)
{
    const bool home = presentation == CardPresentation::Home;
    if (home) {
        // Black placeholder rectangle; artwork is blitted over it when loaded.
        BitmapFont::fillRect(fb, x, y, w, h, 0, 0, 0, 255);
    } else {
        const SDL_Color tint = presentationArtworkColor(item);
        BitmapFont::fillRect(fb, x, y, w, h, tint.r, tint.g, tint.b, tint.a);
    }

    // B5d2b: render loaded row artwork over the placeholder
    {
        std::string key = rowArtworkKey(item);
        if (!key.empty()) {
            auto it = m_rowArtwork.find(key);
            if (it != m_rowArtwork.end() && it->second.status == RowArtworkStatus::Loaded &&
                it->second.image && !it->second.image->empty()) {
                const DecodedImage& img = *it->second.image;
                // Check pre-scaled card surface cache
                char cacheKeyBuf[512];
                std::snprintf(cacheKeyBuf, sizeof(cacheKeyBuf), "%s:%dx%d", key.c_str(), w, h);
                std::string cacheKey(cacheKeyBuf);
                auto cached = m_cardSurfaceCache.find(cacheKey);
                if (cached != m_cardSurfaceCache.end() && cached->second) {
                    // Cache hit — plain blit, no create/scale/free
                    SDL_Surface* cs = cached->second;
                    int drawX = x + (w - cs->w) / 2;
                    int drawY = y + (h - cs->h) / 2;
                    SDL_Rect dstRect = {drawX, drawY, cs->w, cs->h};
                    SDL_BlitSurface(cs, nullptr, fb, &dstRect);
                } else {
                    // Cache miss — scale and blit, then cache for next frame
                    int imgW = img.width;
                    int imgH = img.height;
                    float imgAspect = (float)imgW / (float)imgH;
                    float boxAspect = (float)w / (float)h;
                    int drawW, drawH;
                    if (imgAspect > boxAspect) {
                        drawW = w;
                        drawH = (int)(w / imgAspect + 0.5f);
                        if (drawH > h)
                            drawH = h;
                    } else {
                        drawH = h;
                        drawW = (int)(h * imgAspect + 0.5f);
                        if (drawW > w)
                            drawW = w;
                    }
                    int drawX = x + (w - drawW) / 2;
                    int drawY = y + (h - drawH) / 2;

                    SDL_Surface* imgSurface =
                        SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(), imgW, imgH, 32, imgW * 4,
                                                 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
                    if (imgSurface) {
                        SDL_Rect srcRect = {0, 0, imgW, imgH};
                        SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
                        SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
                        // Cache the pre-scaled surface for subsequent frames
                        prepareCardSurface(cacheKey, img, w, h);
                        SDL_FreeSurface(imgSurface);
                    }
                }
            }
        }
    }

    // In-card black strip keeps every grid card's title readable.  Home cards
    // draw their title/subtitle beneath the card instead.
    if (!home) {
        int ty = y + h - BitmapFont::GLYPH_H - 2;
        BitmapFont::fillRect(fb, x, ty, w, BitmapFont::GLYPH_H + 2, 0, 0, 0, 160);
        int mcc = (w - 4) / BitmapFont::GLYPH_W;
        std::string truncated = BitmapFont::truncateUtf8(item.title, mcc);
        BitmapFont::drawString(fb, x + 2, ty + 1, truncated.c_str(), 255, 255, 255, 0, 0, 0);
    }
    if (selected) {
        if (home) {
            drawFocusGlow(fb, x, y, w, h);
        } else {
            // Movies/Shows grid selection is intentionally unchanged.
            BitmapFont::drawRect(fb, x - 2, y - 2, w + 4, h + 4, 255, 220, 40);
            BitmapFont::drawRect(fb, x - 1, y - 1, w + 2, h + 2, 255, 255, 120);
        }
    } else {
        BitmapFont::drawRect(fb, x, y, w, h, home ? CARD_BORDER_R : Theme::TEXT_R,
                             home ? CARD_BORDER_G : Theme::TEXT_G,
                             home ? CARD_BORDER_B : Theme::TEXT_B);
    }
}

void HomeScreen::drawPlaceholderTab(SDL_Surface* fb, const char* message)
{
    BitmapFont::drawString(fb, 8, 200, message, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void HomeScreen::drawDownloadsTab(SDL_Surface* fb)
{
    BitmapFont::fillRect(fb, 0, 25, 640, 437, 24, 24, 32, 255);
    char summary[128];
    std::snprintf(summary, sizeof(summary), "Free %s | Local %s | Queue %s",
                  formatBytes(m_downloadSnapshot.freeBytes).c_str(),
                  formatBytes(m_downloadSnapshot.localBytes).c_str(),
                  formatBytes(m_downloadSnapshot.reservedBytes).c_str());
    BitmapFont::drawString(fb, 8, 32, summary, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           24, 24, 32);
    BitmapFont::fillRect(fb, 8, 49, 624, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    const auto& rows = m_downloadHierarchy.visible;
    if (rows.empty()) {
        BitmapFont::drawString(fb, 8, 210, "No downloads", Theme::TEXT_R, Theme::TEXT_G,
                               Theme::TEXT_B, 24, 24, 32);
        if (!m_missingJournalEntries.empty())
            BitmapFont::drawString(fb, 8, 230, "Missing offline progress: X=Discard",
                                   Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B, 24,
                                   24, 32);
        return;
    }
    bool moviesLabel = false;
    bool showsLabel = false;
    for (int visible = 0; visible < 5; ++visible) {
        int index = m_downloadScroll + visible;
        if (index >= static_cast<int>(rows.size()))
            break;
        const DownloadHierarchyRow& row = rows[index];
        const DownloadItem* item = row.item;
        int y = 58 + visible * 76;
        bool selected = index == m_downloadSelected;
        const bool movie = row.kind == DownloadHierarchyRowKind::Movie;
        if (movie && !moviesLabel) {
            BitmapFont::drawString(fb, 8, y, "Movies", Theme::ACCENT_R, Theme::ACCENT_G,
                                   Theme::ACCENT_B, 24, 24, 32);
            y += 11;
            moviesLabel = true;
        }
        if (!movie && !showsLabel) {
            BitmapFont::drawString(fb, 8, y, "Shows", Theme::ACCENT_R, Theme::ACCENT_G,
                                   Theme::ACCENT_B, 24, 24, 32);
            y += 11;
            showsLabel = true;
        }
        if (selected)
            BitmapFont::fillRect(fb, 6 + row.indent * 14, y - 2, 628 - row.indent * 14, 58,
                                 Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 80);
        const int x = 12 + row.indent * 14;
        std::string title = row.title;
        if (row.kind == DownloadHierarchyRowKind::Series ||
            row.kind == DownloadHierarchyRowKind::Season)
            title += row.expanded ? "  v" : "  >";
        if (title.size() > 68 - (size_t)row.indent * 2)
            title.resize(68 - (size_t)row.indent * 2);
        BitmapFont::drawString(fb, x, y, title.c_str(), selected ? Theme::ACCENT_R : Theme::TEXT_R,
                               selected ? Theme::ACCENT_G : Theme::TEXT_G,
                               selected ? Theme::ACCENT_B : Theme::TEXT_B, 24, 24, 32);
        std::string detail;
        if (!item) {
            detail = std::to_string(row.aggregate.episodes) + " episodes";
            if (row.aggregate.active)
                detail += " | " + std::to_string(row.aggregate.progress) + "%";
            else if (row.aggregate.complete)
                detail += " | " + std::to_string(row.aggregate.complete) + " complete";
            if (row.aggregate.bytesKnown)
                detail += " | " + formatBytes(row.aggregate.bytes) + "/" +
                          formatBytes(row.aggregate.totalBytes);
        } else {
            const bool completedHls = item->hlsStorage && downloadHierarchyComplete(*item);
            const std::string sizeLabel = (item->hlsStorage && !completedHls ? "~" : "") +
                                          formatBytes(displayDownloadBytes(*item));
            if (item->itemType == "episode")
                detail = episodeDownloadLabel(*item);
            else
                detail = std::string(downloadStateLabel(item->state)) + " | " + sizeLabel;
            if (item->state == DownloadState::Downloading) {
                const std::uint64_t total = predictedDownloadTotalBytes(*item);
                const bool approxTotal = item->hlsStorage && !completedHls;
                char active[96];
                std::snprintf(active, sizeof(active), "Downloading %u%% | %s / %s%s | %s/s",
                              downloadPercent(*item), formatBytes(item->downloadedBytes).c_str(),
                              approxTotal ? "~" : "", formatBytes(total).c_str(),
                              formatBytes(item->recentBytesPerSec).c_str());
                detail = active;
            } else if (item->itemType == "episode") {
                detail += " | " + std::string(downloadStateLabel(item->state)) + " | " + sizeLabel;
            }
            if (!item->lastError.empty() && (item->state == DownloadState::Failed ||
                                             item->state == DownloadState::WaitingForNetwork ||
                                             item->state == DownloadState::Unauthorized))
                detail = std::string(downloadStateLabel(item->state)) + " | " + item->lastError;
        }
        if (detail.size() > 74)
            detail.resize(74);
        BitmapFont::drawString(fb, x, y + 18, detail.c_str(), Theme::TEXT_R, Theme::TEXT_G,
                               Theme::TEXT_B, 24, 24, 32);
        BitmapFont::fillRect(fb, x, y + 39, 600 - row.indent * 14, 3, 48, 48, 58, 255);
        int fill =
            static_cast<int>((600 - row.indent * 14) *
                             (item ? downloadPercent(*item)
                                   : (row.aggregate.progressKnown ? row.aggregate.progress : 0)) /
                             100);
        if (fill)
            BitmapFont::fillRect(fb, x, y + 39, fill, 3, Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G,
                                 Theme::HIGHLIGHT_B, 255);
    }
}

void HomeScreen::drawBottomHints(SDL_Surface* fb)
{
    int y = 480 - BOTTOM_H;
    BitmapFont::fillRect(fb, 0, y, 640, BOTTOM_H, Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                         Theme::BG_B * 2 / 3, 255);

    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb, 8, y + 2, "Y=Logout", Theme::TEXT_R, Theme::TEXT_G,
                               Theme::TEXT_B, Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                               Theme::BG_B * 2 / 3);
    } else if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb, 8, y + 2, "A=Retry  Y=Logout", Theme::TEXT_R, Theme::TEXT_G,
                               Theme::TEXT_B, Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                               Theme::BG_B * 2 / 3);
    } else if (m_logoutArmed && !m_logoutRequested) {
        int secs = (m_logoutTimer + 999) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)", secs);
        BitmapFont::drawString(fb, 8, y + 2, buf, Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G,
                               Theme::HIGHLIGHT_B, Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                               Theme::BG_B * 2 / 3);
    } else {
        if (activeTabNamed("Home")) {
            drawHomeStatusBar(fb, y);
            return;
        }
        const char* hints = "A=Select  B=Back  L/R=Tabs  Y=Logout";
        if (activeTabNamed("Settings")) {
            const char* hint = "Up/Down=Scroll  B=Back  L/R=Tabs";
            if (m_settingsConfirmation == SettingsConfirmation::ChangeServer)
                hint = "A again: Change Server";
            else if (m_settingsConfirmation == SettingsConfirmation::Logout)
                hint = "A again: Log Out";
            BitmapFont::drawString(fb, 8, y + 2, hint, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                                   Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3);
            return;
        }
        if (activeTabNamed("Downloads")) {
            if (!m_journalDiscardConfirmId.empty()) {
                BitmapFont::drawString(fb, 8, y + 2, "Press X again to discard missing progress",
                                       Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
                                       Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                                       Theme::BG_B * 2 / 3);
                return;
            }
            const DownloadHierarchyRow* selectedRow =
                m_downloadSelected >= 0 &&
                        m_downloadSelected < static_cast<int>(m_downloadHierarchy.visible.size())
                    ? &m_downloadHierarchy.visible[m_downloadSelected]
                    : nullptr;
            const DownloadItem* selectedItem = selectedRow ? selectedRow->item : nullptr;
            if (!m_downloadConfirmId.empty() && selectedRow &&
                m_downloadConfirmId == selectedRow->id) {
                char confirm[96];
                if (selectedItem) {
                    const DownloadItem& item = *selectedItem;
                    std::snprintf(confirm, sizeof(confirm), "Press Y again to %s %s",
                                  downloadRemoveIsDelete(item) ? "delete" : "cancel",
                                  formatBytes(displayDownloadBytes(item)).c_str());
                } else {
                    std::snprintf(confirm, sizeof(confirm), "Press Y again to delete %s (%u local)",
                                  selectedRow->kind == DownloadHierarchyRowKind::Season
                                      ? "Season"
                                      : "entire Series",
                                  (unsigned)m_downloadConfirmItemIds.size());
                }
                BitmapFont::drawString(fb, 8, y + 2, confirm, Theme::HIGHLIGHT_R,
                                       Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B, Theme::BG_R * 2 / 3,
                                       Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3);
                return;
            }
            const char* primary = "";
            if (selectedItem)
                primary = downloadPrimaryControlLabel(downloadPrimaryControl(selectedItem->state));
            bool update = selectedItem && selectedItem->state == DownloadState::UpdateAvailable;
            if (selectedRow && !selectedItem)
                primary = selectedRow->expanded ? "Collapse" : "Expand";
            const bool parent = selectedRow && !selectedItem;
            char downloadHints[96];
            std::snprintf(downloadHints, sizeof(downloadHints),
                          update
                              ? "A=Play  X=Update  Y=Delete  B=Back"
                              : (parent ? "A=%s  Y=Delete all  B=Back"
                                        : (!m_missingJournalEntries.empty()
                                               ? "A=%s  X=Discard missing progress  Y=Cancel/Delete"
                                               : "A=%s  Y=Cancel/Delete  B=Back")),
                          primary[0] ? primary : "Select");
            hints = downloadHints;
            BitmapFont::drawString(fb, 8, y + 2, hints, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                                   Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3);
            return;
        }
        if (activeTabNamed("Movies")) {
            hints = m_movieRailFocused ? "A=Filter  Right=Movies  B=Back"
                                       : "A=Select  Left@edge=Alphabet  L/R=Tabs";
        } else if (activeTabNamed("Shows")) {
            hints = m_showsFocus == ShowsFocus::AlphabetRail
                        ? "A=Filter  Right=Shows  B=Back"
                        : "A=Select  Left/Right=Move  Edge=Alphabet";
        }
        BitmapFont::drawString(fb, 8, y + 2, hints, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3);
    }
}

void HomeScreen::drawLoadingState(SDL_Surface* fb)
{
    char userBuf[64];
    std::snprintf(userBuf, sizeof(userBuf), "Logged in as %s", m_userName.c_str());
    int ux = (fb->w - (int)::strlen(userBuf) * BitmapFont::GLYPH_W) / 2;
    int uy = fb->h / 3;
    BitmapFont::drawString(fb, ux, uy, userBuf, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    static int dotPhase = 0;
    dotPhase = (dotPhase + 1) % 60;
    int dots = dotPhase / 15;
    char buf[32] = "Loading library";
    for (int i = 0; i < dots; ++i)
        std::strcat(buf, ".");
    int mx = (fb->w - (int)::strlen(buf) * BitmapFont::GLYPH_W) / 2;
    int my = uy + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, mx, my, buf, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void HomeScreen::drawErrorState(SDL_Surface* fb)
{
    const char* header = "Failed to load library";
    int hx = (fb->w - (int)::strlen(header) * BitmapFont::GLYPH_W) / 2;
    int hy = fb->h / 3;
    BitmapFont::drawString(fb, hx, hy, header, Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G,
                           Theme::HIGHLIGHT_B, Theme::BG_R, Theme::BG_G, Theme::BG_B);

    char errBuf[128];
    std::snprintf(errBuf, sizeof(errBuf), "%s", m_fetchError.c_str());
    int maxChars = (640 - 16) / BitmapFont::GLYPH_W;
    if ((int)::strlen(errBuf) > maxChars)
        errBuf[maxChars] = '\0';
    int ex = (fb->w - (int)::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
    int ey = hy + BitmapFont::GLYPH_H + 8;
    BitmapFont::drawString(fb, ex, ey, errBuf, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    const char* hint = "Press A to retry";
    int hix = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
    int hiy = ey + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, hix, hiy, hint, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
