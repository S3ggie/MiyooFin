#include "HomeScreen.hpp"
#include "../../net/ServerAddress.hpp"
#include "SeriesScreen.hpp"
#include "MovieDetailsScreen.hpp"
#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../ArtworkLayout.hpp"
#include "../MovieTitle.hpp"
#include "../ShowsBrowser.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../net/ArtworkUrl.hpp"
#include "../../net/HttpClient.hpp"
#include "../../net/RouteRequest.hpp"
#include "../../net/RouteStatus.hpp"
#include "../../cache/ImageCache.hpp"
#include "../../app/ScreenStack.hpp"
#include "../../app/UiDiagnostics.hpp"
#include "../../playback/PlaybackRequest.hpp"
#include "../../download/DownloadSupport.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <map>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cctype>
#include <curl/curl.h>

namespace miyoofin {

// Layout constants
static constexpr int TAB_Y       = 0;
static constexpr int TAB_H       = 24;
static constexpr int INFO_Y      = 26;
static constexpr int INFO_H      = 110;
static constexpr int ROWS_Y      = INFO_Y + INFO_H + 2;
static constexpr int BOTTOM_H    = 18;
static constexpr int CARD_GAP    = 6;
static constexpr int ROW_STRIP_H = 96;  // max card height across all types
static constexpr int ROW_LABEL_H = 18;
static constexpr int VISIBLE_ROWS = 3;
static constexpr int POSTER_MAX_CONCURRENT = 4;
static constexpr size_t POSTER_MAX_BYTES = 256 * 1024;
static constexpr int SEASON_POSTER_W = 74;
static constexpr int SEASON_POSTER_H = 111;
static constexpr int MOVIE_GRID_COLUMNS = 8;
static constexpr int MOVIE_GRID_ROWS = 3;
static constexpr int SHOWS_RAIL_W=36, SHOWS_PREVIEW_H=105, SHOWS_GRID_TOP=153;
static constexpr int SHOWS_HALF_W=302, SHOWS_LEFT_X=36, SHOWS_RIGHT_X=338;
static constexpr std::int64_t SYNC_FRESH_WALL_MS=15LL*60*1000;
static constexpr std::int64_t HIERARCHY_RECONCILE_MS=24LL*60*60*1000;
// Selected artwork box origin (top-left of info panel)
static constexpr int ART_X = 8;
static constexpr int ART_Y = INFO_Y + 6;   // 32
static void blitDecoded(SDL_Surface *fb, const DecodedImage &img, int x, int y, int w, int h) {
    if (img.empty()) return;
    float ia=(float)img.width/img.height, ba=(float)w/h;
    int dw=ia>ba?w:(int)(h*ia+.5f), dh=ia>ba?(int)(w/ia+.5f):h;
    SDL_Surface *s=SDL_CreateRGBSurfaceFrom((void*)img.pixels.data(),img.width,img.height,32,img.width*4,0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    if(s){SDL_Rect src={0,0,img.width,img.height},dst={x+(w-dw)/2,y+(h-dh)/2,dw,dh};SDL_BlitScaled(s,&src,fb,&dst);SDL_FreeSurface(s);}
}

void HomeScreen::render(SDL_Surface *fb)
{
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
    const TabData &tab = currentTab();
    if (activeTabNamed("Downloads")) {
        drawDownloadsTab(fb);
    } else if (activeTabNamed("Settings")) {
        drawSettingsTab(fb);
    } else if (!activeTabNamed("Movies") && !activeTabNamed("Shows")
               && tab.rows.size() == 1 && tab.rows[0].items.empty()) {
        drawPlaceholderTab(fb, tab.rows[0].label.empty() ? "No content" : tab.rows[0].label.c_str());
    } else {
        bool hasItems = false;
        for (const auto &r : tab.rows)
            if (!r.items.empty()) { hasItems = true; break; }
        if (activeTabNamed("Movies")) {
            drawMoviePreview(fb); drawMovieAlphabetRail(fb); drawMovieGrid(fb);
        } else if (activeTabNamed("Shows")) {
            drawShowsPreview(fb); drawShowsAlphabetRail(fb); drawShowsGrid(fb);
        } else if (!hasItems) {
            drawPlaceholderTab(fb, tab.name == "Movies" ?
                "No movies on this server" :
                tab.name == "Shows" ? "No shows on this server" : "No content");
        } else {
            drawInfoPanel(fb); drawRowList(fb);
        }
    }
    drawBottomHints(fb);
}
void HomeScreen::drawTabBar(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, TAB_Y, 640, TAB_H,
        Theme::BG_R*2/3, Theme::BG_G*2/3, Theme::BG_B*2/3, 255);
    BitmapFont::fillRect(fb, 0, TAB_Y+TAB_H-1, 640, 1,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 100);
    int x = 8, tabY = TAB_Y + (TAB_H - BitmapFont::GLYPH_H)/2;
    for (int i = 0; i < (int)m_tabs.size(); ++i) {
        const char *name = m_tabs[i].name.c_str();
        if (i == m_activeTab) {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            int tw = (int)::strlen(name) * BitmapFont::GLYPH_W;
            BitmapFont::fillRect(fb, x, TAB_Y+TAB_H-3, tw, 2,
                Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,200);
        } else {
            BitmapFont::drawString(fb,x,tabY,name,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
        }
        x += (int)::strlen(name) * BitmapFont::GLYPH_W + 16;
    }
    std::string status = syncStatusText();
    std::string login = "Logged in as: " + m_userName;
    const int maxChars = 24;
    if ((int)login.size() > maxChars) login = login.substr(0, maxChars - 3) + "...";
    int loginX = 640 - 8 - (int)login.size() * BitmapFont::GLYPH_W;
    // Do not obscure tabs on unusually narrow font/theme combinations.
    if (loginX > x + 4)
        BitmapFont::drawString(fb, loginX, tabY, login.c_str(), Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R*2/3, Theme::BG_G*2/3, Theme::BG_B*2/3);
    int statusX=loginX-8-(int)status.size()*BitmapFont::GLYPH_W;
    if (!status.empty() && statusX > x+4)
        BitmapFont::drawString(fb,statusX,tabY,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,
                               Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
}

std::string HomeScreen::syncStatusText() const
{
    if (!activeTabNamed("Home") && !activeTabNamed("Movies") && !activeTabNamed("Shows"))
        return "";
    return librarySyncStatus(m_activeTab,m_haveCachedSnapshot,
        m_libraryOffline || m_hierarchyOffline.load(),m_syncSchedule.inFlight,
        m_syncSchedule.hasSucceeded,{m_hierarchyCompleted.load(),m_hierarchyTotal.load()},
        m_hierarchyActive.load(),activeTabNamed("Shows"));
}

void HomeScreen::drawInfoPanel(SDL_Surface *fb)
{
    const MediaItem *item = currentItem();
    if (!item) return;
    BitmapFont::fillRect(fb,0,INFO_Y,640,INFO_H,24,24,32,255);
    BitmapFont::fillRect(fb,0,INFO_Y,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
    ArtworkBox box = artworkBoxSize(*item);
    int px=ART_X, py=ART_Y, pw=box.w, ph=box.h;
    // Placeholder colour behind everything
    BitmapFont::fillRect(fb,px,py,pw,ph,item->artR,item->artG,item->artB,255);

    // Render decoded artwork if available, aspect-fit centred
    if (!m_selectedArtwork.empty()) {
        int imgW = m_selectedArtwork.width;
        int imgH = m_selectedArtwork.height;
        float imgAspect = (float)imgW / (float)imgH;
        float boxAspect = (float)pw / (float)ph;
        int drawW, drawH;
        if (imgAspect > boxAspect) {
            // Wider than box — fit to width
            drawW = pw;
            drawH = (int)(pw / imgAspect + 0.5f);
            if (drawH > ph) drawH = ph;
        } else {
            // Taller than box — fit to height
            drawH = ph;
            drawW = (int)(ph * imgAspect + 0.5f);
            if (drawW > pw) drawW = pw;
        }
        int drawX = px + (pw - drawW) / 2;
        int drawY = py + (ph - drawH) / 2;

        // Create an SDL surface wrapping the RGBA pixel data.
        // The DecodedImage (m_selectedArtwork) keeps the pixels alive.
        SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
            (void *)m_selectedArtwork.pixels.data(),
            imgW, imgH,
            32,                    // bits per pixel
            imgW * 4,              // pitch (bytes per row)
            0x000000FF,            // R mask
            0x0000FF00,            // G mask
            0x00FF0000,            // B mask
            0xFF000000);           // A mask
        if (imgSurface) {
            SDL_Rect srcRect = {0, 0, imgW, imgH};
            SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
            SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
            SDL_FreeSurface(imgSurface);
        }
    }

    BitmapFont::drawRect(fb,px,py,pw,ph,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    int mx=px+pw+10, my=py+2;

    // Title: for episodes show series name on the first line and the
    // episode title on a second visually-subordinate line.
    if (item->type == "episode" && !item->seriesName.empty()) {
        BitmapFont::drawString(fb,mx,my,item->seriesName.c_str(),
            Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
        my += BitmapFont::GLYPH_H + 2;
        BitmapFont::drawString(fb,mx,my,item->title.c_str(),
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    } else {
        BitmapFont::drawString(fb,mx,my,item->title.c_str(),
            Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
    }
    my += BitmapFont::GLYPH_H + 2;

    // Metadata: year and/or genre with conditional separator
    {
        char line1[128];
        if (item->year > 0 && !item->genre.empty())
            std::snprintf(line1,sizeof(line1),"%d  |  %s",item->year,item->genre.c_str());
        else if (item->year > 0)
            std::snprintf(line1,sizeof(line1),"%d",item->year);
        else if (!item->genre.empty())
            std::snprintf(line1,sizeof(line1),"%s",item->genre.c_str());
        else
            line1[0] = '\0';
        if (line1[0] != '\0') {
            BitmapFont::drawString(fb,mx,my,line1,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
            my += BitmapFont::GLYPH_H + 2;
        }
    }

    // Rating — Jellyfin community rating is 0.0–10.0
    if (item->rating > 0.0f) {
        char rn[16];
        std::snprintf(rn,sizeof(rn),"%.1f/10",(double)item->rating);
        BitmapFont::drawString(fb,mx,my,rn,
            Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
        my += BitmapFont::GLYPH_H + 2;
    }

    // --- Playback progress bar and status ---
    {
        const bool hasProgress = item->played
            || item->progress > 0.0f
            || item->playbackPositionTicks > 0;
        if (hasProgress) {
            const int pct = item->played ? 100 : playbackPercent(*item);
            constexpr int BAR_W = 180;
            constexpr int BAR_H = 4;
            // Percentage text on the progress row
            const int py = my - 2;  // start 2 px above current my
            char pctBuf[12];
            std::snprintf(pctBuf,sizeof(pctBuf),"%d%%",pct);
            BitmapFont::drawString(fb,mx+BAR_W+4,py,pctBuf,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
            // Track (background) — vertically centred in the text row
            const int barY = py + (BitmapFont::GLYPH_H - BAR_H) / 2;
            BitmapFont::fillRect(fb,mx,barY,BAR_W,BAR_H,40,40,50,255);
            // Filled portion
            const int fillW = BAR_W * std::max(0,std::min(100,pct)) / 100;
            if (fillW > 0)
                BitmapFont::fillRect(fb,mx,barY,fillW,BAR_H,
                    Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,255);
            my = py + BitmapFont::GLYPH_H;

            // Watched/remaining time or "Watched"
            if (item->played) {
                BitmapFont::drawString(fb,mx,my,"Watched",
                    Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
            } else {
                const std::string timeStr =
                    formatPlaybackTime(item->playbackPositionTicks,
                                       item->runTimeTicks);
                if (!timeStr.empty()) {
                    BitmapFont::drawString(fb,mx,my,timeStr.c_str(),
                        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
                }
            }
        }
    }

    BitmapFont::fillRect(fb,0,INFO_Y+INFO_H,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
}

void HomeScreen::drawRowList(SDL_Surface *fb)
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;
    static constexpr int VGAP = 4;
    static constexpr int HMARGIN = 4;
    for (int ri=0; ri<VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int rowY = ROWS_Y + ri * (ROW_LABEL_H + ROW_STRIP_H + VGAP + 4);
        int caY = rowY + ROW_LABEL_H;
        char label[64];
        std::snprintf(label,sizeof(label),"  %s",row.label.c_str());
        BitmapFont::drawString(fb,4,rowY,label,
            rowIdx==m_activeRow?Theme::ACCENT_R:Theme::TEXT_R,
            rowIdx==m_activeRow?Theme::ACCENT_G:Theme::TEXT_G,
            rowIdx==m_activeRow?Theme::ACCENT_B:Theme::TEXT_B,
            Theme::BG_R,Theme::BG_G,Theme::BG_B);
        // Draw cards with per-item sizing and pixel-scroll offset
        int cardAccumX = HMARGIN;
        for (int ci=0; ci<(int)row.items.size(); ++ci) {
            ArtworkBox sz = artworkBoxSize(row.items[ci]);
            int screenX = cardAccumX - m_cardScroll;
            if (screenX + sz.w < HMARGIN) {
                // Fully off-screen left
                cardAccumX += sz.w + CARD_GAP;
                continue;
            }
            if (screenX > 640 - HMARGIN) break;
            bool sel = (rowIdx==m_activeRow && ci==m_activeCard);
            int cardScreenY = caY + (ROW_STRIP_H - sz.h) / 2;
            drawCard(fb,screenX,cardScreenY,sz.w,sz.h,row.items[ci],sel);
            cardAccumX += sz.w + CARD_GAP;
        }
    }
}

void HomeScreen::drawMovieGrid(SDL_Surface *fb)
{
    const MediaRow *row = currentRow(); if (!row) return;
    const int top = 134, gap = 6;
    for (int i=0; i<(int)row->items.size(); ++i) {
        int gr = i / MOVIE_GRID_COLUMNS, gc = i % MOVIE_GRID_COLUMNS;
        if (gr < m_rowScroll || gr >= m_rowScroll + MOVIE_GRID_ROWS) continue;
        int x = 42 + gc * (64 + gap), y = top + (gr - m_rowScroll) * (96 + gap);
        drawCard(fb, x, y, 64, 96, row->items[i], !m_movieRailFocused && i == m_activeCard);
    }
    if (row->items.empty() && m_movieActiveLetter >= 0) {
        char message[48]; std::snprintf(message, sizeof(message), "No movies starting with %c", 'A' + m_movieActiveLetter);
        BitmapFont::drawString(fb, 48, 210, message, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B, Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }
}

void HomeScreen::drawMovieAlphabetRail(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, 25, 36, 437, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 35, 25, 1, 437, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    for (int i = 0; i < 26; ++i) {
        int y = 27 + i * 16;
        bool focused = m_movieRailFocused && i == m_movieAlphabetFocus;
        bool active = i == m_movieActiveLetter;
        if (focused) BitmapFont::fillRect(fb, 2, y - 1, 31, BitmapFont::GLYPH_H + 2, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 120);
        char letter[2] = {static_cast<char>('A' + i), '\0'};
        BitmapFont::drawString(fb, 14, y, letter,
            active ? Theme::HIGHLIGHT_R : focused ? Theme::BG_R : Theme::TEXT_R,
            active ? Theme::HIGHLIGHT_G : focused ? Theme::BG_G : Theme::TEXT_G,
            active ? Theme::HIGHLIGHT_B : focused ? Theme::BG_B : Theme::TEXT_B,
            focused ? Theme::ACCENT_R : 24, focused ? Theme::ACCENT_G : 24, focused ? Theme::ACCENT_B : 32);
        if (active && !focused) BitmapFont::fillRect(fb, 4, y + BitmapFont::GLYPH_H + 1, 27, 1, Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B, 255);
    }
}

void HomeScreen::drawMoviePreview(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 36, 25, 604, 105, 24, 24, 32, 255);
    BitmapFont::fillRect(fb, 36, 129, 604, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 70);
    const MediaItem *item = currentItem();
    if (!item) return;
    int px = 42, py = 29;
    BitmapFont::fillRect(fb, px, py, 64, 96, item->artR, item->artG, item->artB, 255);
    if (!m_selectedArtwork.empty()) {
        SDL_Surface *image = SDL_CreateRGBSurfaceFrom((void *)m_selectedArtwork.pixels.data(), m_selectedArtwork.width, m_selectedArtwork.height, 32, m_selectedArtwork.width * 4, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
        if (image) { SDL_Rect src={0,0,m_selectedArtwork.width,m_selectedArtwork.height}, dst={px,py,64,96}; SDL_BlitScaled(image,&src,fb,&dst); SDL_FreeSurface(image); }
    }
    BitmapFont::drawRect(fb, px, py, 64, 96, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
    int x=114, y=33;
    std::string truncatedTitle = BitmapFont::truncateUtf8(item->title, 65);
    BitmapFont::drawString(fb, x, y, truncatedTitle.c_str(), Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 24,24,32);
    char meta[96] = {}; int n=0;
    if (item->year > 0) n += std::snprintf(meta+n, sizeof(meta)-n, "%d", item->year);
    int mins=ticksToMinutes(item->runTimeTicks); if (mins > 0) n += std::snprintf(meta+n, sizeof(meta)-n, "%s%dh %dm", n ? " * " : "", mins/60, mins%60);
    if (item->rating > 0) std::snprintf(meta+n, sizeof(meta)-n, "%s%.1f", n ? " * " : "", (double)item->rating);
    BitmapFont::drawString(fb, x, y+18, meta, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,24,24,32);
    char state[96]; std::snprintf(state, sizeof(state), "%s%s", item->genre.c_str(), item->played ? (item->genre.empty()?"Watched":" * Watched") : item->progress > 0 ? "" : "");
    if (item->progress > 0 && !item->played) std::snprintf(state, sizeof(state), "%s%s%d%% watched", item->genre.c_str(), item->genre.empty()?"":" * ", (int)(item->progress*100.0f+0.5f));
    BitmapFont::drawString(fb, x, y+36, state, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,24,24,32);
}

void HomeScreen::drawShowsAlphabetRail(SDL_Surface *fb) {
    BitmapFont::fillRect(fb,0,25,SHOWS_RAIL_W,437,24,24,32,255); BitmapFont::fillRect(fb,35,25,1,437,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,90);
    for(int i=0;i<26;++i){int y=27+i*16;bool f=m_showsFocus==ShowsFocus::AlphabetRail&&i==m_showsAlphabetFocus,a=i==m_showsActiveLetter;if(f)BitmapFont::fillRect(fb,2,y-1,31,BitmapFont::GLYPH_H+2,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,120);char c[2]={char('A'+i),0};BitmapFont::drawString(fb,14,y,c,a?Theme::HIGHLIGHT_R:f?Theme::BG_R:Theme::TEXT_R,a?Theme::HIGHLIGHT_G:f?Theme::BG_G:Theme::TEXT_G,a?Theme::HIGHLIGHT_B:f?Theme::BG_B:Theme::TEXT_B,f?Theme::ACCENT_R:24,f?Theme::ACCENT_G:24,f?Theme::ACCENT_B:32);}
}
void HomeScreen::drawShowsPreview(SDL_Surface *fb) {
    BitmapFont::fillRect(fb,36,25,604,SHOWS_PREVIEW_H,24,24,32,255); BitmapFont::fillRect(fb,36,129,604,1,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,70); const MediaItem*item=showsSelectedItem();if(!item)return;int px=42,py=29;BitmapFont::fillRect(fb,px,py,64,96,item->artR,item->artG,item->artB,255); std::string key=rowArtworkKey(*item);auto it=m_rowArtwork.find(key);if(it!=m_rowArtwork.end()&&it->second.status==RowArtworkStatus::Loaded&&it->second.image)blitDecoded(fb,*it->second.image,px,py,64,96);else blitDecoded(fb,m_selectedArtwork,px,py,64,96);BitmapFont::drawRect(fb,px,py,64,96,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);BitmapFont::drawString(fb,114,33,item->title.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);char meta[96]={};int n=0;if(item->year)n+=std::snprintf(meta+n,sizeof(meta)-n,"%d",item->year);if(item->rating>0)std::snprintf(meta+n,sizeof(meta)-n,"%s%.1f",n?" * ":"",(double)item->rating);BitmapFont::drawString(fb,114,51,meta,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);char state[96];std::snprintf(state,sizeof(state),"%s%s",item->genre.c_str(),item->played?" * Watched":item->progress>0?" * In progress":"");BitmapFont::drawString(fb,114,69,state,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
}
void HomeScreen::drawShowsGrid(SDL_Surface *fb) {
    BitmapFont::drawString(fb,44,137,"SHOWS",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);BitmapFont::drawString(fb,346,137,"ANIME",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);BitmapFont::fillRect(fb,337,135,1,327,Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,100);auto draw=[&](const std::vector<MediaItem>&v,int scroll,int sel,bool focused,int base){for(int i=0;i<(int)v.size();++i){int r=i/4;if(r<scroll||r>=scroll+3)continue;drawCard(fb,base+14+(i%4)*70,SHOWS_GRID_TOP+(r-scroll)*102,64,96,v[i],focused&&i==sel);}};draw(m_filteredShows,m_showScroll,m_showSelected,m_showsFocus==ShowsFocus::ShowsGrid,SHOWS_LEFT_X);draw(m_filteredAnime,m_animeScroll,m_animeSelected,m_showsFocus==ShowsFocus::AnimeGrid,SHOWS_RIGHT_X);if(m_filteredShows.empty()&&m_filteredAnime.empty()){char b[64];if(m_showsActiveLetter>=0)std::snprintf(b,sizeof(b),"No shows or anime starting with %c",'A'+m_showsActiveLetter);else std::snprintf(b,sizeof(b),"No shows on this server");BitmapFont::drawString(fb,48,230,b,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);}
}

void HomeScreen::drawCard(SDL_Surface *fb,int x,int y,int w,int h,
                          const MediaItem &item,bool selected)
{
    BitmapFont::fillRect(fb,x,y,w,h,item.artR,item.artG,item.artB,255);

    // B5d2b: render loaded row artwork over the placeholder
    {
        std::string key = rowArtworkKey(item);
        if (!key.empty()) {
            auto it = m_rowArtwork.find(key);
            if (it != m_rowArtwork.end()
                && it->second.status == RowArtworkStatus::Loaded
                && it->second.image
                && !it->second.image->empty())
            {
                const DecodedImage &img = *it->second.image;
                int imgW = img.width;
                int imgH = img.height;
                float imgAspect = (float)imgW / (float)imgH;
                float boxAspect = (float)w / (float)h;
                int drawW, drawH;
                if (imgAspect > boxAspect) {
                    drawW = w;
                    drawH = (int)(w / imgAspect + 0.5f);
                    if (drawH > h) drawH = h;
                } else {
                    drawH = h;
                    drawW = (int)(h * imgAspect + 0.5f);
                    if (drawW > w) drawW = w;
                }
                int drawX = x + (w - drawW) / 2;
                int drawY = y + (h - drawH) / 2;

                SDL_Surface *imgSurface = SDL_CreateRGBSurfaceFrom(
                    (void *)img.pixels.data(),
                    imgW, imgH,
                    32,
                    imgW * 4,
                    0x000000FF,
                    0x0000FF00,
                    0x00FF0000,
                    0xFF000000);
                if (imgSurface) {
                    SDL_Rect srcRect = {0, 0, imgW, imgH};
                    SDL_Rect dstRect = {drawX, drawY, drawW, drawH};
                    SDL_BlitScaled(imgSurface, &srcRect, fb, &dstRect);
                    SDL_FreeSurface(imgSurface);
                }
            }
        }
    }

    int ty = y + h - BitmapFont::GLYPH_H - 2;
    BitmapFont::fillRect(fb,x,ty,w,BitmapFont::GLYPH_H+2,0,0,0,160);
    int mcc = (w-4)/BitmapFont::GLYPH_W;
    std::string truncated = BitmapFont::truncateUtf8(item.title, mcc);
    BitmapFont::drawString(fb,x+2,ty+1,truncated.c_str(),255,255,255,0,0,0);
    if (selected) {
        BitmapFont::drawRect(fb,x-2,y-2,w+4,h+4,255,220,40);
        BitmapFont::drawRect(fb,x-1,y-1,w+2,h+2,255,255,120);
    } else {
        BitmapFont::drawRect(fb,x,y,w,h,
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    }
}

void HomeScreen::drawPlaceholderTab(SDL_Surface *fb, const char *message)
{
    BitmapFont::drawString(fb,8,200,message,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
        Theme::BG_R,Theme::BG_G,Theme::BG_B);
}

void HomeScreen::drawDownloadsTab(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 0, 25, 640, 437, 24, 24, 32, 255);
    char summary[128];
    std::snprintf(summary, sizeof(summary), "Free %s | Local %s | Queue %s",
        formatBytes(m_downloadSnapshot.freeBytes).c_str(),
        formatBytes(m_downloadSnapshot.localBytes).c_str(),
        formatBytes(m_downloadSnapshot.reservedBytes).c_str());
    BitmapFont::drawString(fb, 8, 32, summary, Theme::ACCENT_R, Theme::ACCENT_G,
        Theme::ACCENT_B, 24, 24, 32);
    BitmapFont::fillRect(fb, 8, 49, 624, 1, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 90);
    const auto &rows=m_downloadHierarchy.visible;
    if (rows.empty()) {
        BitmapFont::drawString(fb, 8, 210, "No downloads", Theme::TEXT_R, Theme::TEXT_G,
            Theme::TEXT_B, 24, 24, 32);
        if (!m_missingJournalEntries.empty()) BitmapFont::drawString(fb,8,230,"Missing offline progress: X=Discard",Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
        return;
    }
    bool moviesLabel=false, showsLabel=false;
    for (int visible=0; visible<5; ++visible) {
        int index=m_downloadScroll+visible; if(index >= (int)rows.size()) break;
        const DownloadHierarchyRow &row=rows[index]; const DownloadItem *item=row.item;
        int y=58+visible*76; bool selected=index==m_downloadSelected;
        const bool movie=row.kind==DownloadHierarchyRowKind::Movie;
        if(movie&&!moviesLabel) { BitmapFont::drawString(fb,8,y,"Movies",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32); y+=11; moviesLabel=true; }
        if(!movie&&!showsLabel) { BitmapFont::drawString(fb,8,y,"Shows",Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32); y+=11; showsLabel=true; }
        if(selected) BitmapFont::fillRect(fb, 6+row.indent*14, y-2, 628-row.indent*14, 58, Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B, 80);
        const int x=12+row.indent*14;
        std::string title=row.title;
        if(row.kind==DownloadHierarchyRowKind::Series || row.kind==DownloadHierarchyRowKind::Season) title+=(row.expanded?"  v":"  >");
        if(title.size()>68-(size_t)row.indent*2) title.resize(68-(size_t)row.indent*2);
        BitmapFont::drawString(fb, x, y, title.c_str(), selected?Theme::ACCENT_R:Theme::TEXT_R,
            selected?Theme::ACCENT_G:Theme::TEXT_G, selected?Theme::ACCENT_B:Theme::TEXT_B,24,24,32);
        std::string detail;
        if(!item) {
            detail=std::to_string(row.aggregate.episodes)+" episodes";
            if(row.aggregate.active) detail+=" | "+std::to_string(row.aggregate.progress)+"%";
            else if(row.aggregate.complete) detail+=" | "+std::to_string(row.aggregate.complete)+" complete";
            if(row.aggregate.bytesKnown) detail+=" | "+formatBytes(row.aggregate.bytes)+"/"+formatBytes(row.aggregate.totalBytes);
        } else {
        const bool completedHls=item->hlsStorage && downloadHierarchyComplete(*item);
        const std::string sizeLabel=(item->hlsStorage&&!completedHls?"~":"")+formatBytes(displayDownloadBytes(*item));
        if(item->itemType=="episode") detail=episodeDownloadLabel(*item);
        else detail=std::string(downloadStateLabel(item->state))+" | "+sizeLabel;
        if(item->state==DownloadState::Downloading) {
            char active[96]; std::snprintf(active,sizeof(active),"Downloading %u%% | %s/s",downloadPercent(*item),formatBytes(item->recentBytesPerSec).c_str()); detail=active;
        } else if(item->itemType=="episode") detail += " | "+std::string(downloadStateLabel(item->state))+" | "+sizeLabel;
        if(!item->lastError.empty() && (item->state==DownloadState::Failed || item->state==DownloadState::WaitingForNetwork || item->state==DownloadState::Unauthorized))
            detail=std::string(downloadStateLabel(item->state))+" | "+item->lastError;
        }
        if(detail.size()>74) detail.resize(74);
        BitmapFont::drawString(fb, x, y+18, detail.c_str(), Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
        BitmapFont::fillRect(fb, x, y+39, 600-row.indent*14, 3, 48,48,58,255);
        int fill=(int)((600-row.indent*14)*(item?downloadPercent(*item):(row.aggregate.progressKnown?row.aggregate.progress:0))/100);
        if(fill) BitmapFont::fillRect(fb,x,y+39,fill,3,Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,255);
    }
}

void HomeScreen::drawBottomHints(SDL_Surface *fb)
{
    int y = 480 - BOTTOM_H;
    BitmapFont::fillRect(fb,0,y,640,BOTTOM_H,
        Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3,255);

    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb,8,y+2,"Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb,8,y+2,"A=Retry  Y=Logout",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else if (m_logoutArmed && !m_logoutRequested) {
        int secs = (m_logoutTimer + 999) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)", secs);
        BitmapFont::drawString(fb,8,y+2,buf,
            Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else {
        const char *hints = "A=Select  B=Back  L/R=Tabs  Y=Logout";
        if (activeTabNamed("Settings")) {
            const char *hint = "Up/Down=Scroll  B=Back  L/R=Tabs";
            if (m_settingsConfirmation == SettingsConfirmation::ChangeServer)
                hint = "A again: Change Server";
            else if (m_settingsConfirmation == SettingsConfirmation::Logout)
                hint = "A again: Log Out";
            BitmapFont::drawString(fb,8,y+2,hint,
                Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
                Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            return;
        }
        if (activeTabNamed("Downloads")) {
            if (!m_journalDiscardConfirmId.empty()) {
                BitmapFont::drawString(fb,8,y+2,"Press X again to discard missing progress",Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
                return;
            }
            const DownloadHierarchyRow *selectedRow=m_downloadSelected>=0&&m_downloadSelected<(int)m_downloadHierarchy.visible.size()?&m_downloadHierarchy.visible[m_downloadSelected]:nullptr;
            const DownloadItem *selectedItem=selectedRow?selectedRow->item:nullptr;
            if (!m_downloadConfirmId.empty() && selectedRow && m_downloadConfirmId==selectedRow->id) {
                char confirm[96];
                if (selectedItem) {
                    const DownloadItem &item=*selectedItem;
                    std::snprintf(confirm,sizeof(confirm),"Press Y again to %s %s",downloadRemoveIsDelete(item)?"delete":"cancel",formatBytes(displayDownloadBytes(item)).c_str());
                } else {
                    std::snprintf(confirm,sizeof(confirm),"Press Y again to delete %s (%u local)",selectedRow->kind==DownloadHierarchyRowKind::Season?"Season":"entire Series",(unsigned)m_downloadConfirmItemIds.size());
                }
                BitmapFont::drawString(fb,8,y+2,confirm,Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
                return;
            }
            const char *primary="";
            if (selectedItem) primary=downloadPrimaryControlLabel(downloadPrimaryControl(selectedItem->state));
            bool update=selectedItem && selectedItem->state==DownloadState::UpdateAvailable;
            if(selectedRow && !selectedItem) primary=selectedRow->expanded?"Collapse":"Expand";
            const bool parent=selectedRow && !selectedItem;
            char downloadHints[96]; std::snprintf(downloadHints,sizeof(downloadHints),update?"A=Play  X=Update  Y=Delete  B=Back":(parent?"A=%s  Y=Delete all  B=Back":(!m_missingJournalEntries.empty()?"A=%s  X=Discard missing progress  Y=Cancel/Delete":"A=%s  Y=Cancel/Delete  B=Back")),primary[0]?primary:"Select"); hints=downloadHints;
            BitmapFont::drawString(fb,8,y+2,hints,Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
            return;
        }
        if (activeTabNamed("Movies")) {
            hints = m_movieRailFocused
                ? "A=Filter  Right=Movies  B=Back"
                : "A=Select  Left@edge=Alphabet  L/R=Tabs";
        } else if (activeTabNamed("Shows")) {
            hints = m_showsFocus == ShowsFocus::AlphabetRail
                ? "A=Filter  Right=Shows  B=Back"
                : "A=Select  Left/Right=Move  Edge=Alphabet";
        }
        BitmapFont::drawString(fb,8,y+2, hints,
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    }
}

void HomeScreen::drawLoadingState(SDL_Surface *fb)
{
    char userBuf[64];
    std::snprintf(userBuf, sizeof(userBuf), "Logged in as %s",
                  m_userName.c_str());
    int ux = (fb->w - (int)::strlen(userBuf) * BitmapFont::GLYPH_W) / 2;
    int uy = fb->h / 3;
    BitmapFont::drawString(fb, ux, uy, userBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    static int dotPhase = 0;
    dotPhase = (dotPhase + 1) % 60;
    int dots = dotPhase / 15;
    char buf[32] = "Loading library";
    for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
    int mx = (fb->w - (int)::strlen(buf) * BitmapFont::GLYPH_W) / 2;
    int my = uy + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, mx, my, buf,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void HomeScreen::drawErrorState(SDL_Surface *fb)
{
    const char *header = "Failed to load library";
    int hx = (fb->w - (int)::strlen(header) * BitmapFont::GLYPH_W) / 2;
    int hy = fb->h / 3;
    BitmapFont::drawString(fb, hx, hy, header,
        Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    char errBuf[128];
    std::snprintf(errBuf, sizeof(errBuf), "%s", m_fetchError.c_str());
    int maxChars = (640 - 16) / BitmapFont::GLYPH_W;
    if ((int)::strlen(errBuf) > maxChars) errBuf[maxChars] = '\0';
    int ex = (fb->w - (int)::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
    int ey = hy + BitmapFont::GLYPH_H + 8;
    BitmapFont::drawString(fb, ex, ey, errBuf,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    const char *hint = "Press A to retry";
    int hix = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
    int hiy = ey + BitmapFont::GLYPH_H + 16;
    BitmapFont::drawString(fb, hix, hiy, hint,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
