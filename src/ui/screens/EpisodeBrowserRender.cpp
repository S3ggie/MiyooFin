#include "EpisodeBrowserScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../download/DownloadSupport.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

static constexpr int FB_W=640, FB_H=480, BOTTOM_H=18;
static constexpr int LEFT_X=16, HEAD_Y=16, LIST_Y=46, LIST_W=285, LIST_ROW_H=18;
static constexpr int THUMB_X=326, THUMB_Y=38, THUMB_W=288, THUMB_H=162;
static constexpr int META_X=326, META_Y=THUMB_Y+THUMB_H+6, META_WRAP=34;
static constexpr int BTN_W=78, BTN_H=20, BTN_Y=FB_H-BOTTOM_H-BTN_H-6;
static constexpr int BTN_PLAY_X=326, BTN_EP_X=410, BTN_SEASON_X=494;
static constexpr Uint8 FOCUS_OR=255, FOCUS_OG=220, FOCUS_OB=40;
static constexpr Uint8 FOCUS_IR=255, FOCUS_IG=255, FOCUS_IB=120;

static std::vector<std::string> wrapText(const char *text, int wrapCols)
{
    std::vector<std::string> lines; if (!text || !*text) return lines;
    std::string input(text); size_t pos=0;
    while (pos<input.size()) {
        size_t newline=input.find('\n',pos);
        std::string para=newline!=std::string::npos?input.substr(pos,newline-pos):input.substr(pos);
        while(!para.empty()){
            if((int)para.size()<=wrapCols){lines.push_back(para);break;}
            size_t lastSpace=para.rfind(' ',wrapCols);
            if(lastSpace!=std::string::npos&&lastSpace>0){lines.push_back(para.substr(0,lastSpace));para=para.substr(lastSpace+1);}
            else{lines.push_back(para.substr(0,wrapCols));para=para.substr(wrapCols);}
        }
        if(newline!=std::string::npos)pos=newline+1;else break;
    }
    return lines;
}

static void renderBottomHints(SDL_Surface *fb, const char *hint)
{
    int y = FB_H - BOTTOM_H;
    BitmapFont::fillRect(fb, 0, y, FB_W, BOTTOM_H,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3, 255);
    BitmapFont::drawString(fb, 8, y + 2, hint,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
        Theme::BG_B * 2 / 3);
}

static std::string formatEpNum(int indexNumber)
{
    if (indexNumber <= 0) return {};
    char buf[16];
    std::snprintf(buf, sizeof(buf), "E%02d", indexNumber);
    return std::string(buf);
}

void EpisodeBrowserScreen::render(SDL_Surface *fb)
{
    // --- Loading state ---
    if (m_loadState == LoadState::Loading) {
        BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        const char *msg = "Loading episodes...";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, mx, FB_H / 3, msg,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "B=Back");
        return;
    }
    // --- Error state ---
    if (m_loadState == LoadState::Error) {
        BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        const char *hdr = "Failed to load episodes";
        int hx = (FB_W - (int)std::strlen(hdr) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, hx, FB_H / 3, hdr,
            Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        char errBuf[128];
        std::snprintf(errBuf, sizeof(errBuf), "%s", m_error.c_str());
        int maxC = (FB_W - 32) / BitmapFont::GLYPH_W;
        if ((int)std::strlen(errBuf) > maxC) errBuf[maxC] = '\0';
        int ex = (FB_W - (int)std::strlen(errBuf) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, ex, FB_H / 3 + BitmapFont::GLYPH_H + 8,
            errBuf, Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "A=Retry  B=Back");
        return;
    }

    // === Ready: left panel ===
    int total = (int)m_episodes.size();
    BitmapFont::drawString(fb, LEFT_X, HEAD_Y, "EPISODES",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // --- Left panel: episode list rows ---
    for (int vis = 0; vis < LIST_VISIBLE; ++vis) {
        int idx = m_listScroll + vis;
        if (idx >= total) break;
        int ry = LIST_Y + vis * LIST_ROW_H;
        const auto &ep = m_episodes[idx];

        std::string epNum = formatEpNum(ep.indexNumber);
        std::string label = epNum.empty()
            ? ep.title : epNum + "  " + ep.title;

        int maxChars = LIST_W / BitmapFont::GLYPH_W;
        if ((int)label.size() > maxChars) {
            if (maxChars > 3) {
                label[maxChars - 1] = '.';
                label[maxChars - 2] = '.';
                label[maxChars - 3] = '.';
            }
            label.resize(maxChars > 0 ? maxChars : 1);
        }

        bool isSel = (idx == m_selectedEpisode);
        bool isFocused = (m_focus == FocusArea::EpisodeList && isSel);

        if (isFocused) {
            BitmapFont::fillRect(fb, LEFT_X - 2, ry - 1,
                LIST_W + 4, LIST_ROW_H, FOCUS_OR, FOCUS_OG, FOCUS_OB, 255);
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                0, 0, 0, FOCUS_OR, FOCUS_OG, FOCUS_OB);
        } else if (isSel) {
            BitmapFont::fillRect(fb, LEFT_X - 2, ry - 1,
                LIST_W + 4, LIST_ROW_H,
                Theme::ACCENT_R / 2, Theme::ACCENT_G / 2,
                Theme::ACCENT_B / 2, 255);
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::ACCENT_R / 2, Theme::ACCENT_G / 2,
                Theme::ACCENT_B / 2);
        } else {
            BitmapFont::drawString(fb, LEFT_X, ry, label.c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
        }
    }

    if (m_listScroll > 0)
        BitmapFont::drawString(fb, LEFT_X + LIST_W - 16, LIST_Y - 2,
            "^", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
    if (m_listScroll + LIST_VISIBLE < total)
        BitmapFont::drawString(fb, LEFT_X + LIST_W - 16,
            LIST_Y + LIST_VISIBLE * LIST_ROW_H,
            "v", Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // === Right panel ===
    if (total <= 0 || m_selectedEpisode < 0 || m_selectedEpisode >= total) {
        const char *msg = "No episodes";
        int mx = (FB_W - (int)std::strlen(msg) * BitmapFont::GLYPH_W) / 2;
        BitmapFont::drawString(fb, mx, FB_H / 3, msg,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        renderBottomHints(fb, "B=Back");
        return;
    }

    const auto &ep = m_episodes[m_selectedEpisode];

    if (m_downloads && m_planId) {
        auto p=m_downloads->planSnapshot(m_planId); std::string status;
        if(m_confirmDownload) status=std::string("Download ")+(m_planIsSeason?"Season "+std::to_string(m_season.indexNumber):"Episode")+"?";
        else if(p.state==DownloadPlanState::Planning) status=(p.plan.sizeKnown?"~"+formatBytes(p.plan.additionalRequiredBytes)+"  Preparing download...":"Preparing download...");
        else if(p.state==DownloadPlanState::Ready) status=std::to_string(p.itemCount)+" episodes  ~"+formatBytes(p.plan.additionalRequiredBytes)+" needed  "+formatBytes(p.plan.usableFreeBytes)+" free";
        else if(p.state==DownloadPlanState::Error) status=p.plan.error;
        if(!status.empty()) BitmapFont::drawString(fb,META_X,BTN_Y-28,status.c_str(),Theme::ACCENT_R,Theme::ACCENT_G,Theme::ACCENT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
        if(m_confirmDownload) BitmapFont::drawString(fb,META_X,BTN_Y-14,"A=Confirm  B=Cancel",Theme::TEXT_R,Theme::TEXT_G,Theme::TEXT_B,Theme::BG_R,Theme::BG_G,Theme::BG_B);
    }

    // Placeholder thumbnail (coloured box as fallback/background)
    BitmapFont::fillRect(fb, THUMB_X, THUMB_Y, THUMB_W, THUMB_H,
        ep.artR, ep.artG, ep.artB, 255);

    if (m_episodeArtworkSurface.surface) {
        SDL_Rect dstRect = {m_episodeArtworkSurface.x,
                            m_episodeArtworkSurface.y, 0, 0};
        SDL_BlitSurface(m_episodeArtworkSurface.surface, nullptr, fb, &dstRect);
    }

    // Thumbnail border (drawn after artwork)
    BitmapFont::drawRect(fb, THUMB_X, THUMB_Y, THUMB_W, THUMB_H,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);

    // Episode title
    int my = META_Y;
    std::string titleStr = ep.title;
    int maxTC = (FB_W - META_X - 8) / BitmapFont::GLYPH_W;
    if ((int)titleStr.size() > maxTC) {
        if (maxTC > 3) {
            titleStr[maxTC-1]='.';
            titleStr[maxTC-2]='.';
            titleStr[maxTC-3]='.';
        }
        titleStr.resize(maxTC);
    }
    BitmapFont::drawString(fb, META_X, my, titleStr.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    my += BitmapFont::GLYPH_H + 2;

    // Metadata: "S1 E3 | 11 min | 8.4/10"
    bool hasMeta = false;
    char metaBuf[128]; metaBuf[0] = '\0';
    if (ep.parentIndexNumber > 0 && ep.indexNumber > 0) {
        std::snprintf(metaBuf, sizeof(metaBuf), "S%d E%d",
                      ep.parentIndexNumber, ep.indexNumber);
        hasMeta = true;
    }
    if (ep.runTimeTicks > 0) {
        int mins = ticksToMinutes(ep.runTimeTicks);
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf+len, sizeof(metaBuf)-len,
                          " | %d min", mins);
        else { std::snprintf(metaBuf, sizeof(metaBuf), "%d min", mins); hasMeta = true; }
    }
    if (ep.rating > 0.0f) {
        int len = (int)std::strlen(metaBuf);
        if (hasMeta)
            std::snprintf(metaBuf+len, sizeof(metaBuf)-len,
                          " | %.1f/10", (double)ep.rating);
        else
            std::snprintf(metaBuf, sizeof(metaBuf), "%.1f/10", (double)ep.rating);
    }
    if (hasMeta) {
        BitmapFont::drawString(fb, META_X, my, metaBuf,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        my += BitmapFont::GLYPH_H + 2;
    }

    // Overview (word-wrapped, scrollable)
    bool overviewScrollable = false;
    if (!ep.overview.empty()) {
        auto lines = wrapText(ep.overview.c_str(), META_WRAP);
        int vis = (BTN_Y - 4 - my) / BitmapFont::GLYPH_H;
        if (vis < 1) vis = 1;
        int maxScroll = (int)lines.size() - vis;
        if (maxScroll < 0) maxScroll = 0;
        if (m_overviewScroll > maxScroll) m_overviewScroll = maxScroll;
        if (m_overviewScroll < 0) m_overviewScroll = 0;
        overviewScrollable = (maxScroll > 0);
        int drawY = my;
        for (int i = m_overviewScroll;
             i < m_overviewScroll + vis && i < (int)lines.size(); ++i) {
            BitmapFont::drawString(fb, META_X, drawY, lines[i].c_str(),
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                Theme::BG_R, Theme::BG_G, Theme::BG_B);
            drawY += BitmapFont::GLYPH_H;
        }
    }

    // --- Action buttons ---
    auto drawBtn = [&](int bx, const char *label, bool focused) {
        if (focused) {
            BitmapFont::fillRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_OR, FOCUS_OG, FOCUS_OB, 255);
            BitmapFont::drawRect(fb, bx - 2, BTN_Y - 2,
                BTN_W + 4, BTN_H + 4, FOCUS_OR, FOCUS_OG, FOCUS_OB);
            BitmapFont::drawRect(fb, bx - 1, BTN_Y - 1,
                BTN_W + 2, BTN_H + 2, FOCUS_IR, FOCUS_IG, FOCUS_IB);
        } else {
            BitmapFont::fillRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3,
                Theme::BG_B * 2 / 3, 255);
            BitmapFont::drawRect(fb, bx, BTN_Y, BTN_W, BTN_H,
                Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B);
        }
        int tw = (int)std::strlen(label) * BitmapFont::GLYPH_W;
        int tx = bx + (BTN_W - tw) / 2;
        int ty = BTN_Y + (BTN_H - BitmapFont::GLYPH_H) / 2;
        BitmapFont::drawString(fb, tx, ty, label,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            focused ? FOCUS_OR : (Theme::BG_R * 2 / 3),
            focused ? FOCUS_OG : (Theme::BG_G * 2 / 3),
            focused ? FOCUS_OB : (Theme::BG_B * 2 / 3));
    };

    bool playFocused = (m_focus == FocusArea::ActionButtons
                        && m_actionBtn == ActionButton::Play);
    bool dlFocused = (m_focus == FocusArea::ActionButtons
                      && m_actionBtn == ActionButton::DownloadEpisode);
    bool seasonFocused = (m_focus == FocusArea::ActionButtons
                      && m_actionBtn == ActionButton::DownloadSeason);
    drawBtn(BTN_PLAY_X, "PLAY", playFocused);
    drawBtn(BTN_EP_X, "EPISODE", dlFocused);
    drawBtn(BTN_SEASON_X, "SEASON", seasonFocused);

    // Bottom hint bar
    renderBottomHints(fb, overviewScrollable
        ? "A=Select B=Back Y=Season L/R=Bio" : "A=Select B=Back Y=Season");
}


} // namespace miyoofin
