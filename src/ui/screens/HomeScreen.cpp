#include "HomeScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>
#include <map>

namespace miyoofin {

// Layout constants
static constexpr int TAB_Y       = 0;
static constexpr int TAB_H       = 24;
static constexpr int INFO_Y      = 26;
static constexpr int INFO_H      = 110;
static constexpr int ROWS_Y      = INFO_Y + INFO_H + 2;
static constexpr int BOTTOM_H    = 18;
static constexpr int CARD_W      = 160;
static constexpr int CARD_H      = 90;
static constexpr int CARD_GAP    = 6;
static constexpr int ROW_LABEL_H = 18;
static constexpr int VISIBLE_ROWS = 3;

HomeScreen::HomeScreen(const Session &session)
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_session(session)
    , m_userName(session.userName)
{
    // Placeholder tabs until fetch completes
    m_tabs.push_back({"Home", {{"", {}}}});
    m_tabs.push_back({"Movies", {{"", {}}}});
    m_tabs.push_back({"Shows", {{"", {}}}});
    m_tabs.push_back({"Search", {{"", {}}}});
    m_tabs.push_back({"Downloads", {{"", {}}}});
}

HomeScreen::~HomeScreen()
{
    if (m_fetchThread.joinable())
        m_fetchThread.join();
}

const TabData &HomeScreen::currentTab() const
{
    return m_tabs[m_activeTab];
}

const MediaRow *HomeScreen::currentRow() const
{
    const auto &rows = currentTab().rows;
    if (m_activeRow < (int)rows.size())
        return &rows[m_activeRow];
    return nullptr;
}

const MediaItem *HomeScreen::currentItem() const
{
    const MediaRow *row = currentRow();
    if (row && m_activeCard < (int)row->items.size())
        return &row->items[m_activeCard];
    return nullptr;
}

void HomeScreen::clampNavigation()
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) {
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        return;
    }
    if (m_activeRow < 0) m_activeRow = 0;
    if (m_activeRow >= (int)rows.size()) m_activeRow = (int)rows.size() - 1;
    const auto &items = rows[m_activeRow].items;
    if (items.empty()) {
        m_activeCard = 0; m_cardScroll = 0;
        return;
    }
    if (m_activeCard < 0) m_activeCard = 0;
    if (m_activeCard >= (int)items.size()) m_activeCard = (int)items.size() - 1;
    if (m_activeRow < m_rowScroll) m_rowScroll = m_activeRow;
    if (m_activeRow >= m_rowScroll + VISIBLE_ROWS)
        m_rowScroll = m_activeRow - VISIBLE_ROWS + 1;
    int visibleCards = (640 - 12) / (CARD_W + CARD_GAP);
    if (m_activeCard < m_cardScroll) m_cardScroll = m_activeCard;
    if (m_activeCard >= m_cardScroll + visibleCards)
        m_cardScroll = m_activeCard - visibleCards + 1;
}

void HomeScreen::enter()
{
    printf("[HomeScreen] enter (tab=%d) user=%s\n", m_activeTab,
           m_userName.c_str());
    if (m_loadState == LoadState::Loading && !m_fetchDone)
        startFetch();
}

void HomeScreen::leave()
{
    printf("[HomeScreen] leave\n");
}

bool HomeScreen::handleAction(Action action)
{
    if (m_logoutArmed && action != Action::ActionsMenu) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
    }

    // Loading: only allow logout
    if (m_loadState == LoadState::Loading) {
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Error: allow retry (A) and logout (Y)
    if (m_loadState == LoadState::Error) {
        if (action == Action::Confirm) {
            m_loadState = LoadState::Loading;
            m_fetchDone = false; m_fetchError.clear(); m_fetchResult.clear();
            startFetch(); return true;
        }
        if (action == Action::ActionsMenu && !m_logoutArmed) {
            m_logoutArmed = true; m_logoutTimer = 3000; return true;
        }
        return false;
    }

    // Ready: normal navigation
    switch (action) {
    case Action::Up:     m_activeRow--; clampNavigation(); return true;
    case Action::Down:   m_activeRow++; clampNavigation(); return true;
    case Action::Left:   m_activeCard--; clampNavigation(); return true;
    case Action::Right:  m_activeCard++; clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::Search:
        m_activeTab = 3; m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0; return true;
    case Action::ActionsMenu:
        if (m_logoutArmed) { m_logoutRequested = true; }
        else { m_logoutArmed = true; m_logoutTimer = 3000; }
        return true;
    case Action::Confirm: {
        const MediaItem *item = currentItem();
        if (item) printf("[HomeScreen] Select: %s (%s)\n",
                         item->title.c_str(), item->type.c_str());
        return true;
    }
    case Action::Back:
        if (m_logoutArmed) { m_logoutArmed = false; m_logoutTimer = 0; return true; }
        return false;
    default: return false;
    }
}

void HomeScreen::update(Uint32 dt)
{
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) { m_logoutTimer = 0; m_logoutArmed = false; }
        else m_logoutTimer -= dt;
    }
    if (m_loadState == LoadState::Loading && m_fetchDone)
        finishFetch();
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
    if (m_activeTab == 3 || m_activeTab == 4) {
        drawPlaceholderTab(fb, m_activeTab == 3 ?
            "Search - not yet implemented" :
            "Downloads - not yet implemented");
    } else if (tab.rows.size() == 1 && tab.rows[0].items.empty()
               && tab.rows[0].label.empty()) {
        drawPlaceholderTab(fb, "No content");
    } else {
        bool hasItems = false;
        for (const auto &r : tab.rows)
            if (!r.items.empty()) { hasItems = true; break; }
        if (!hasItems) {
            drawPlaceholderTab(fb, tab.name == "Movies" ?
                "No movies on this server" :
                tab.name == "Shows" ? "No shows on this server" : "No content");
        } else {
            drawInfoPanel(fb);
            drawRowList(fb);
        }
    }
    drawBottomHints(fb);
}

void HomeScreen::startFetch()
{
    m_fetchDone = false;
    m_fetchError.clear();
    m_fetchResult.clear();

    std::string url   = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid   = m_session.userId;
    std::string devId = m_session.deviceId;

    m_fetchThread = std::thread([this, url, token, uid, devId]() {
        std::string err;

        // 1. Get library views
        std::vector<LibraryView> views;
        if (!JellyfinApi::getViews(url, token, uid, devId, views, err)) {
            m_fetchError = err;
            m_fetchDone = true;
            return;
        }
        printf("[HomeScreen] Got %zu library views\n", views.size());

        // 2. Continue watching (non-fatal if fails)
        std::vector<MediaItem> cw;
        std::string cwErr;
        if (!JellyfinApi::getResumeItems(url, token, uid, devId, 12, cw, cwErr))
            printf("[HomeScreen] Continue watching: %s\n", cwErr.c_str());

        // 3. Recently added (non-fatal)
        std::vector<MediaItem> ra;
        std::string raErr;
        if (!JellyfinApi::getLatestItems(url, token, uid, devId, 16, ra, raErr))
            printf("[HomeScreen] Recently added: %s\n", raErr.c_str());

        // 4. Per-library items
        std::vector<std::pair<std::string, std::vector<MediaItem>>> moviesByView;
        std::vector<std::pair<std::string, std::vector<MediaItem>>> showsByView;
        for (const auto &v : views) {
            if (v.collectionType == "movies") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Movie", 50, items, ie))
                    moviesByView.push_back({v.name, std::move(items)});
                else printf("[HomeScreen] Movies '%s': %s\n",
                            v.name.c_str(), ie.c_str());
            } else if (v.collectionType == "tvshows") {
                std::vector<MediaItem> items; std::string ie;
                if (JellyfinApi::getLibraryItems(url, token, uid, devId,
                        v.id, "Series", 50, items, ie))
                    showsByView.push_back({v.name, std::move(items)});
                else printf("[HomeScreen] Shows '%s': %s\n",
                            v.name.c_str(), ie.c_str());
            }
        }

        m_fetchResult = JellyfinApi::buildTabs(
            views, cw, ra, moviesByView, showsByView);
        m_fetchDone = true;
    });
    m_fetchThread.detach();
}

void HomeScreen::finishFetch()
{
    m_fetchDone = false;
    if (!m_fetchError.empty()) {
        m_loadState = LoadState::Error;
        printf("[HomeScreen] Fetch failed: %s\n", m_fetchError.c_str());
        return;
    }
    m_tabs = std::move(m_fetchResult);
    m_loadState = LoadState::Ready;
    clampNavigation();
    printf("[HomeScreen] Library loaded: %zu tabs\n", m_tabs.size());
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
}

void HomeScreen::drawInfoPanel(SDL_Surface *fb)
{
    const MediaItem *item = currentItem();
    if (!item) return;
    BitmapFont::fillRect(fb,0,INFO_Y,640,INFO_H,24,24,32,255);
    BitmapFont::fillRect(fb,0,INFO_Y,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
    int px=8, py=INFO_Y+6, pw=72, ph=INFO_H-12;
    BitmapFont::fillRect(fb,px,py,pw,ph,item->artR,item->artG,item->artB,255);
    BitmapFont::drawRect(fb,px,py,pw,ph,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B);
    int mx=px+pw+10, my=py+2;
    BitmapFont::drawString(fb,mx,my,item->title.c_str(),
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char line1[128];
    std::snprintf(line1,sizeof(line1),"%d  |  %s",item->year,item->genre.c_str());
    BitmapFont::drawString(fb,mx,my,line1,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char rs[32];
    int fs=(int)item->rating, idx=0;
    for (int s=0;s<fs;++s) rs[idx++]='*';
    if (item->rating-fs>=0.25f) rs[idx++]='.';
    rs[idx]='\0';
    BitmapFont::drawString(fb,mx,my,rs,
        Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,24,24,32);
    char rn[16];
    std::snprintf(rn,sizeof(rn),"  %.1f/5.0",item->rating);
    BitmapFont::drawString(fb,mx+(int)::strlen(rs)*BitmapFont::GLYPH_W,my,rn,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    my += BitmapFont::GLYPH_H + 2;
    char ov[256];
    std::snprintf(ov,sizeof(ov),"%s",item->overview.c_str());
    if ((int)::strlen(ov) > 55) { ov[55]='\0'; std::strcat(ov,"..."); }
    BitmapFont::drawString(fb,mx,my,ov,
        Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,24,24,32);
    BitmapFont::fillRect(fb,0,INFO_Y+INFO_H,640,1,
        Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,60);
}

void HomeScreen::drawRowList(SDL_Surface *fb)
{
    const auto &rows = currentTab().rows;
    if (rows.empty()) return;
    static constexpr int VGAP = 4;
    for (int ri=0; ri<VISIBLE_ROWS; ++ri) {
        int rowIdx = m_rowScroll + ri;
        if (rowIdx >= (int)rows.size()) break;
        const MediaRow &row = rows[rowIdx];
        int rowY = ROWS_Y + ri * (ROW_LABEL_H + CARD_H + VGAP + 4);
        int caY = rowY + ROW_LABEL_H;
        char label[64];
        std::snprintf(label,sizeof(label),"  %s",row.label.c_str());
        BitmapFont::drawString(fb,4,rowY,label,
            rowIdx==m_activeRow?Theme::ACCENT_R:Theme::TEXT_R,
            rowIdx==m_activeRow?Theme::ACCENT_G:Theme::TEXT_G,
            rowIdx==m_activeRow?Theme::ACCENT_B:Theme::TEXT_B,
            Theme::BG_R,Theme::BG_G,Theme::BG_B);
        int cx = 4;
        for (int ci=m_cardScroll; ci<(int)row.items.size(); ++ci) {
            if (cx+CARD_W > 640-4) break;
            bool sel = (rowIdx==m_activeRow && ci==m_activeCard);
            drawCard(fb,cx,caY,CARD_W,CARD_H,row.items[ci],sel);
            cx += CARD_W + CARD_GAP;
        }
    }
}

void HomeScreen::drawCard(SDL_Surface *fb,int x,int y,int w,int h,
                          const MediaItem &item,bool selected)
{
    BitmapFont::fillRect(fb,x,y,w,h,item.artR,item.artG,item.artB,255);
    int ty = y + h - BitmapFont::GLYPH_H - 2;
    BitmapFont::fillRect(fb,x,ty,w,BitmapFont::GLYPH_H+2,0,0,0,160);
    char buf[64];
    std::snprintf(buf,sizeof(buf),"%s",item.title.c_str());
    int mcc = (w-4)/BitmapFont::GLYPH_W;
    if ((int)::strlen(buf) > mcc) {
        buf[mcc-1]='.'; buf[mcc-2]='.'; buf[mcc]='\0';
    }
    BitmapFont::drawString(fb,x+2,ty+1,buf,255,255,255,0,0,0);
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
        BitmapFont::drawString(fb,8,y+2,
            "A=Select  B=Back  L/R=Tabs  X=Search  Y=Logout",
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
