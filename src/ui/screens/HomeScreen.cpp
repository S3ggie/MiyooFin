#include "HomeScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include <cstdio>
#include <cstring>

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

HomeScreen::HomeScreen()
    : m_activeTab(0), m_activeRow(0), m_activeCard(0)
    , m_rowScroll(0), m_cardScroll(0)
    , m_tabs(getMockTabs())
    , m_logoutArmed(false), m_logoutTimer(0), m_logoutRequested(false)
{
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
    printf("[HomeScreen] enter (tab=%d)\n", m_activeTab);
}

void HomeScreen::leave()
{
    printf("[HomeScreen] leave\n");
}

bool HomeScreen::handleAction(Action action)
{
    // If we're waiting for logout confirmation and user does
    // anything other than Y again, disarm.
    if (m_logoutArmed && action != Action::ActionsMenu) {
        m_logoutArmed = false;
        m_logoutTimer = 0;
    }

    switch (action) {
    case Action::Up:     m_activeRow--; clampNavigation(); return true;
    case Action::Down:   m_activeRow++; clampNavigation(); return true;
    case Action::Left:   m_activeCard--; clampNavigation(); return true;
    case Action::Right:  m_activeCard++; clampNavigation(); return true;
    case Action::NextTab:
        m_activeTab = (m_activeTab + 1) % (int)m_tabs.size();
        m_activeRow = 0; m_activeCard = 0; m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::PrevTab:
        m_activeTab--;
        if (m_activeTab < 0) m_activeTab = (int)m_tabs.size() - 1;
        m_activeRow = 0; m_activeCard = 0; m_rowScroll = 0; m_cardScroll = 0;
        clampNavigation(); return true;
    case Action::Search:
        m_activeTab = 3; m_activeRow = 0; m_activeCard = 0;
        m_rowScroll = 0; m_cardScroll = 0; return true;
    case Action::ActionsMenu:
        if (m_logoutArmed) {
            m_logoutRequested = true;
        } else {
            m_logoutArmed = true;
            m_logoutTimer = 3000;
        }
        return true;
    case Action::Settings:
        printf("[HomeScreen] Settings (not implemented)\n"); return true;
    case Action::Menu:
        printf("[HomeScreen] Menu (not implemented)\n"); return true;
    case Action::Confirm: {
        const MediaItem *item = currentItem();
        if (item)
            printf("[HomeScreen] Select: %s (%s)\n",
                   item->title.c_str(), item->type.c_str());
        return true;
    }
    case Action::Back:
        printf("[HomeScreen] Back (not implemented)\n"); return true;
    default: return false;
    }
}

void HomeScreen::update(Uint32 dt)
{
    if (m_logoutArmed && !m_logoutRequested) {
        if (dt >= m_logoutTimer) {
            m_logoutTimer = 0;
            m_logoutArmed = false;
        } else {
            m_logoutTimer -= dt;
        }
    }
}

void HomeScreen::render(SDL_Surface *fb)
{
    drawTabBar(fb);
    if (m_activeTab == 3)
        drawPlaceholderTab(fb, "Search - not yet implemented");
    else if (m_activeTab == 4)
        drawPlaceholderTab(fb, "Downloads - not yet implemented");
    else {
        drawInfoPanel(fb);
        drawRowList(fb);
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

    if (m_logoutArmed && !m_logoutRequested) {
        int secs = (m_logoutTimer + 999) / 1000;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Press Y again to confirm logout (%d)", secs);
        BitmapFont::drawString(fb,8,y+2,buf,
            Theme::HIGHLIGHT_R,Theme::HIGHLIGHT_G,Theme::HIGHLIGHT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    } else {
        BitmapFont::drawString(fb,8,y+2,
            "A=Select  B=Back  L/R=Tabs  X=Search  Y=Logout  START=Settings  SELECT=Menu",
            Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,
            Theme::BG_R*2/3,Theme::BG_G*2/3,Theme::BG_B*2/3);
    }
}

} // namespace miyoofin
