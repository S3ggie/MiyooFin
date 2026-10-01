#include "../UiKit.hpp"
#include "ConnectScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>
#include <utility>

namespace miyoofin {

ConnectScreen::ConnectScreen(const std::string& savedUrl) : m_savedUrl(savedUrl) {}

#ifdef MIYOOFIN_TEST_BUILD
ConnectScreen::ConnectScreen(const std::string& savedUrl, ConnectionAttempt attempt)
    : m_savedUrl(savedUrl), m_connectionAttempt(std::move(attempt))
{}
#endif

ConnectScreen::~ConnectScreen()
{
    stopConnection();
}

void ConnectScreen::stopConnection()
{
    m_connectWorker.cancel();
    m_connectWorker.join();
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
    // ScreenStack retires this screen on its bounded cleanup worker. Signal
    // cancellation here, but do not make the SDL thread wait for curl.
    m_connectWorker.cancel();
}

bool ConnectScreen::handleAction(Action action)
{
    (void)action;
    return false; // Exit handled by App
}

void ConnectScreen::update(Uint32 dt)
{
    if (m_finished)
        return;

    if (m_connected) {
        m_finished = true;
        return;
    }

    if (m_failed) {
        m_finished = true;
        return;
    }

    if (m_connectWorker.reap()) {
        finishConnection();
        return;
    }

    if (m_retryTimer > 0) {
        if (dt >= m_retryTimer) {
            m_retryTimer = 0;
            startConnection();
        } else {
            m_retryTimer -= dt;
        }
    }
}

void ConnectScreen::startConnection()
{
    // Reclaim a completed attempt before a retry. A live attempt is refused
    // so retries can never create competing workers.
    if (m_connectWorker.busy() && !m_connectWorker.reap())
        return;

    m_connectSuccess = false;
    m_connectError.clear();
    m_message = "Connecting...";

    std::string url = m_savedUrl;
#ifdef MIYOOFIN_TEST_BUILD
    const auto connectionAttempt = m_connectionAttempt;
#endif
    m_connectWorker.start([this, url
#ifdef MIYOOFIN_TEST_BUILD
                           ,
                           connectionAttempt
#endif
    ](const CancelToken& cancellation) {
        ServerInfo info;
        std::string err;
        bool ok = false;
#ifdef MIYOOFIN_TEST_BUILD
        if (connectionAttempt)
            ok = connectionAttempt(url, info, err, cancellation.get());
        else
#endif
            ok = JellyfinApi::getSystemInfo(url, info, err, cancellation.get());
        if (ok) {
            m_connectResult = info;
            m_connectSuccess = true;
        } else {
            m_connectError = err;
            m_connectSuccess = false;
        }
    });
}

void ConnectScreen::finishConnection()
{
    // update() reaped the finished worker, so its result fields are visible
    // and retries own no stale joinable worker.
    if (m_connectSuccess) {
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_infoTimer = 0;
        printf("[ConnectScreen] Connected to %s (%s v%s)\n", m_savedUrl.c_str(),
               m_serverInfo.serverName.c_str(), m_serverInfo.version.c_str());
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
        printf("[ConnectScreen] Giving up after retries: %s\n", m_error.c_str());
    }
}

void ConnectScreen::render(SDL_Surface* fb)
{
    ui::SplashSpec spec;
    spec.address = m_savedUrl;
    if (m_connected) {
        spec.headline = "Connected to " + m_serverInfo.serverName;
        spec.headlineColor = design::kSuccess;
        spec.detail = "Jellyfin " + m_serverInfo.version + "  -  starting in " +
                      std::to_string((m_infoTimer + 999) / 1000) + "...";
    } else {
        spec.headline = m_message;
        spec.headlineColor = m_failed ? design::kDanger : design::kText;
        spec.busy = !m_failed;
    }
    spec.hints = {{ui::Key::Menu, "Exit"}};
    ui::splash(fb, spec);
}

} // namespace miyoofin
