#include "ConnectScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

ConnectScreen::ConnectScreen(const std::string &savedUrl)
    : m_savedUrl(savedUrl)
{
}

ConnectScreen::~ConnectScreen()
{
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
}

void ConnectScreen::enter()
{
    printf("[ConnectScreen] enter url=%s\n", m_savedUrl.c_str());
    m_message = "Connecting...";
    startConnection();
}

void ConnectScreen::leave()
{
    printf("[ConnectScreen] leave\n");
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
}

bool ConnectScreen::handleAction(Action action)
{
    (void)action;
    return false;  // Exit handled by App
}

void ConnectScreen::update(Uint32 dt)
{
    if (m_finished) return;

    if (m_connected) { m_finished = true; return; }

    if (m_failed) {
        m_finished = true;
        return;
    }

    if (m_connectDone) {
        finishConnection();
        return;
    }

    if (m_retryTimer > 0) {
        if (dt >= m_retryTimer) { m_retryTimer = 0; startConnection(); }
        else { m_retryTimer -= dt; }
    }
}

void ConnectScreen::startConnection()
{
    m_connectDone = false;
    m_connectSuccess = false;
    m_connectError.clear();
    m_message = "Connecting...";

    std::string url = m_savedUrl;
    m_connectThread = std::thread([this, url]() {
        ServerInfo info;
        std::string err;
        bool ok = JellyfinApi::getSystemInfo(url, info, err);
        if (ok) { m_connectResult = info; m_connectSuccess = true; }
        else { m_connectError = err; m_connectSuccess = false; }
        m_connectDone = true;
    });
    m_connectThread.detach();
}

void ConnectScreen::finishConnection()
{
    m_connectDone = false;

    if (m_connectSuccess) {
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_infoTimer = 0;
        printf("[ConnectScreen] Connected to %s (%s v%s)\n",
               m_savedUrl.c_str(), m_serverInfo.serverName.c_str(),
               m_serverInfo.version.c_str());
        return;
    }

    m_retriesLeft--;
    if (m_retriesLeft > 0) {
        m_retryTimer = 1500;
        m_message = "Connection failed - retrying...";
    } else {
        m_failed = true;
        m_error = m_connectError;
        m_message = "Could not reach server";
        printf("[ConnectScreen] Giving up after retries: %s\n",
               m_error.c_str());
    }
}

void ConnectScreen::render(SDL_Surface *fb)
{
    BitmapFont::drawString(fb, 8, 8, "MiyooFin",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    if (m_connected) {
        char line1[128];
        std::snprintf(line1, sizeof(line1), "Connected to %s",
                      m_serverInfo.serverName.c_str());
        int l1x = (fb->w - (int)::strlen(line1) * BitmapFont::GLYPH_W) / 2;
        int l1y = fb->h / 3;
        BitmapFont::drawString(fb, l1x, l1y, line1,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

        char line2[128];
        std::snprintf(line2, sizeof(line2), "Jellyfin version %s",
                      m_serverInfo.version.c_str());
        int l2x = (fb->w - (int)::strlen(line2) * BitmapFont::GLYPH_W) / 2;
        int l2y = l1y + BitmapFont::GLYPH_H + 4;
        BitmapFont::drawString(fb, l2x, l2y, line2,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

        char line3[128];
        std::snprintf(line3, sizeof(line3), "Server: %s", m_savedUrl.c_str());
        int l3x = (fb->w - (int)::strlen(line3) * BitmapFont::GLYPH_W) / 2;
        int l3y = l2y + BitmapFont::GLYPH_H + 4;
        BitmapFont::drawString(fb, l3x, l3y, line3,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

        char hint[64];
        int secs = (m_infoTimer + 999) / 1000;
        std::snprintf(hint, sizeof(hint), "Starting in %d...", secs);
        int hx = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
        int hy = l3y + BitmapFont::GLYPH_H + 16;
        BitmapFont::drawString(fb, hx, hy, hint,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        return;
    }

    int msgLen = (int)::strlen(m_message.c_str());
    int msgX = (fb->w - msgLen * BitmapFont::GLYPH_W) / 2;
    int msgY = fb->h / 3;
    BitmapFont::drawString(fb, msgX, msgY, m_message.c_str(),
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    int urlLen = (int)m_savedUrl.size();
    int urlX = (fb->w - urlLen * BitmapFont::GLYPH_W) / 2;
    int urlY = msgY + BitmapFont::GLYPH_H + 8;
    BitmapFont::drawString(fb, urlX, urlY, m_savedUrl.c_str(),
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    if (!m_connectDone && !m_failed) {
        static int dotPhase = 0;
        dotPhase = (dotPhase + 1) % 60;
        int dots = dotPhase / 15;
        char buf[32] = "Connecting";
        for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
        int bufLen = (int)::strlen(buf);
        int bufX = (fb->w - bufLen * BitmapFont::GLYPH_W) / 2;
        int bufY = urlY + BitmapFont::GLYPH_H + 12;
        BitmapFont::drawString(fb, bufX, bufY, buf,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }

    int hintY = fb->h - BitmapFont::GLYPH_H - 8;
    BitmapFont::drawString(fb, 8, hintY, "MENU = Exit",
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin
