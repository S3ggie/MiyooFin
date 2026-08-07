#include "AuthCheckScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

AuthCheckScreen::AuthCheckScreen(const Session &session)
    : m_session(session)
    , m_userName(session.userName)
{
}

AuthCheckScreen::~AuthCheckScreen()
{
    if (m_checkThread.joinable()) {
        m_checkThread.join();
    }
}

void AuthCheckScreen::enter()
{
    printf("[AuthCheckScreen] enter user=%s\n", m_userName.c_str());
    m_message = "Checking session...";
    startCheck();
}

void AuthCheckScreen::leave()
{
    if (m_checkThread.joinable()) {
        m_checkThread.join();
    }
}

bool AuthCheckScreen::handleAction(Action action)
{
    (void)action;
    return false;  // Exit handled by App
}

void AuthCheckScreen::update(Uint32 dt)
{
    if (m_finished) return;

    if (m_ok && m_welcomeTimer > 0) {
        if (dt >= m_welcomeTimer) { m_welcomeTimer = 0; m_finished = true; }
        else { m_welcomeTimer -= dt; }
        return;
    }

    if (m_checkDone) {
        finishCheck();
        return;
    }
}

void AuthCheckScreen::startCheck()
{
    m_checkDone = false;
    m_checkSuccess = false;
    m_checkError.clear();

    std::string url = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    m_checkThread = std::thread([this, url, token, uid, devId]() {
        std::string err;
        bool ok = JellyfinApi::validateToken(url, token, uid, devId, err);
        if (ok) { m_checkSuccess = true; }
        else { m_checkError = err; }
        m_checkDone = true;
    });
}

void AuthCheckScreen::finishCheck()
{
    m_checkDone = false;

    if (m_checkSuccess) {
        m_ok = true;
        m_welcomeTimer = 1500;
        printf("[AuthCheckScreen] Session valid for user '%s'\n",
               m_userName.c_str());
        return;
    }

    m_ok = false;
    m_error = m_checkError;
    m_finished = true;
    printf("[AuthCheckScreen] Session invalid: %s\n", m_error.c_str());
}

void AuthCheckScreen::render(SDL_Surface *fb)
{
    BitmapFont::drawString(fb, 8, 8, "MiyooFin",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);

    if (m_ok) {
        char line[160];
        std::snprintf(line, sizeof(line), "Welcome back, %s", m_userName.c_str());
        int lx = (fb->w - (int)::strlen(line) * BitmapFont::GLYPH_W) / 2;
        int ly = fb->h / 3;
        BitmapFont::drawString(fb, lx, ly, line,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);

        char hint[64];
        int secs = (m_welcomeTimer + 999) / 1000;
        std::snprintf(hint, sizeof(hint), "Starting in %d...", secs);
        int hx = (fb->w - (int)::strlen(hint) * BitmapFont::GLYPH_W) / 2;
        int hy = ly + BitmapFont::GLYPH_H + 16;
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

    if (!m_checkDone) {
        static int dotPhase = 0;
        dotPhase = (dotPhase + 1) % 60;
        int dots = dotPhase / 15;
        char buf[32] = "Checking";
        for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
        int bufLen = (int)::strlen(buf);
        int bufX = (fb->w - bufLen * BitmapFont::GLYPH_W) / 2;
        int bufY = msgY + BitmapFont::GLYPH_H + 12;
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