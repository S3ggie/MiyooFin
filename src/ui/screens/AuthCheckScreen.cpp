#include "../UiKit.hpp"
#include "AuthCheckScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

AuthCheckScreen::AuthCheckScreen(const Session& session)
    : m_session(session), m_userName(session.userName)
{}

AuthCheckScreen::~AuthCheckScreen()
{
    m_checkWorker.join();
}

void AuthCheckScreen::enter()
{
    printf("[AuthCheckScreen] enter user=%s\n", m_userName.c_str());
    m_message = "Checking session...";
    startCheck();
}

void AuthCheckScreen::leave()
{
    m_checkWorker.join();
}

bool AuthCheckScreen::handleAction(Action action)
{
    (void)action;
    return false; // Exit handled by App
}

void AuthCheckScreen::update(Uint32 /*dt*/)
{
    if (m_finished)
        return;

    if (m_ok) {
        m_finished = true;
        return;
    }

    if (m_checkWorker.reap()) {
        finishCheck();
        return;
    }
}

void AuthCheckScreen::startCheck()
{
    m_checkSuccess = false;
    m_checkError.clear();

    std::string url = m_session.serverUrl;
    std::string token = m_session.accessToken;
    std::string uid = m_session.userId;
    std::string devId = m_session.deviceId;

    m_checkWorker.start([this, url, token, uid, devId](const CancelToken&) {
        std::string err;
        bool ok = JellyfinApi::validateToken(url, token, uid, devId, err);
        if (ok) {
            m_checkSuccess = true;
        } else {
            m_checkError = err;
        }
    });
}

void AuthCheckScreen::finishCheck()
{
    if (m_checkSuccess) {
        m_ok = true;
        m_welcomeTimer = 0;
        printf("[AuthCheckScreen] Session valid for user '%s'\n", m_userName.c_str());
        return;
    }

    m_ok = false;
    m_error = m_checkError;
    m_finished = true;
    printf("[AuthCheckScreen] Session invalid: %s\n", m_error.c_str());
}

void AuthCheckScreen::render(SDL_Surface* fb)
{
    ui::SplashSpec spec;
    if (m_ok) {
        spec.headline = "Welcome back, " + m_userName;
        spec.detail = "Starting in " + std::to_string((m_welcomeTimer + 999) / 1000) + "...";
    } else {
        spec.headline = m_message;
        spec.busy = m_checkWorker.busy();
    }
    spec.hints = {{ui::Key::Menu, "Exit"}};
    ui::splash(fb, spec);
}

} // namespace miyoofin
