#include "App.hpp"
#include "../ui/screens/HomeScreen.hpp"
#include "../ui/screens/LoginScreen.hpp"
#include "../net/RouteRequest.hpp"
#include "UiDiagnostics.hpp"
#include <cstdio>
#include <cstring>
#include <thread>

namespace miyoofin {

void App::startSavedSessionValidation()
{
    const Session session = m_session;
    m_savedValidation = 0;
    m_savedSessionServerId.clear();
    m_savedValidationThread = std::thread([this, session] {
        std::string error;
        TokenValidation result=TokenValidation::Unavailable;
        RouteRequest(session).run([&](const std::string &base){ result=JellyfinApi::validateTokenStatus(base,session.accessToken,session.userId,session.deviceId,error); return result==TokenValidation::Valid; },error);
        if (result == TokenValidation::Valid && session.serverId.empty()) {
            ServerInfo info;
            std::string systemInfoError;
            if (RouteRequest(session).run([&](const std::string &base){ return JellyfinApi::getSystemInfo(base, info, systemInfoError); }, systemInfoError)) {
                m_savedSessionServerId = info.serverId;
            }
        }
        m_savedValidation = result == TokenValidation::Unauthorized ? 2 :
            (result == TokenValidation::Valid ? 3 : 1);
    });
}

void App::finishSavedSessionValidation()
{
    if (!m_savedFastPath || m_savedValidation.load() == 0) return;
    if (m_savedValidationThread.joinable()) m_savedValidationThread.join();
    const bool unauthorized = m_savedValidation.load() == 2;
    m_savedFastPath = false;
    if (unauthorized) {
        printf("[App] Saved session rejected; returning to login\n");
        logout();
        goToLogin("Session expired. Please log in again.");
    } else if (m_savedValidation.load() == 3) {
        if (m_session.serverId.empty() && !m_savedSessionServerId.empty()) {
            m_session.serverId = m_savedSessionServerId;
            m_session.save();
        }
        scheduleJournalSync();
    }
}

void App::loadSavedUrl()
{
    FILE *f = fopen("server.txt", "r");
    if (!f) return;
    char buf[512];
    if (fgets(buf, sizeof(buf), f)) {
        buf[511] = '\0';
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')) {
            buf[--len] = '\0';
        }
        m_serverUrl = buf;
    }
    fclose(f);
}

void App::configureCatalogScopeForSession()
{
    if (m_catalogDb && m_session.valid()) {
        uiDiagnostics().log("[App] startup stage=catalog_scope_start");
        uiDiagnostics().log("[App] catalog scope request identity=valid");
        m_catalogScopeEpoch =
            m_catalogDb->configureScope(m_session.serverUrl, m_session.userId);
        m_librarySync = std::make_shared<library::LibrarySync>(
            m_session, m_catalogDb, m_catalogScopeEpoch);
        m_librarySync->startLiveEvents();
        m_libraryQuery = std::make_shared<library::LibraryQuery>(
            m_catalogDb, m_catalogScopeEpoch);
        if (m_downloadManager)
            m_downloadManager->setLibraryServices(m_libraryQuery,
                                                   m_librarySync);
        uiDiagnostics().log("[App] startup stage=catalog_scope_requested");
    } else if (m_catalogDb) {
        uiDiagnostics().log("[App] catalog scope request identity=invalid");
        m_catalogScopeEpoch = m_catalogDb->deconfigureScope();
        m_librarySync.reset();
        m_libraryQuery.reset();
        if (m_downloadManager)
            m_downloadManager->setLibraryServices({}, {});
    }
}

void App::loadSavedSession()
{
    m_session = Session::load();
    if (m_session.valid()) {
        uiDiagnostics().log("[App] saved session valid=1");
        printf("[App] Loaded saved session for user '%s'\n",
               m_session.userName.c_str());
        // Prefer the session's server URL if a server URL is not yet known
        if (m_serverUrl.empty() && !m_session.serverUrl.empty()) {
            m_serverUrl = m_session.serverUrl;
        }
    } else {
        uiDiagnostics().log("[App] saved session valid=0");
        printf("[App] No valid saved session\n");
    }
}
void App::goToHome()
{
    if (m_stack.size() > 1) {
        m_stack.pop();
    }
    m_stack.push(std::make_unique<HomeScreen>(
        m_session, m_downloadManager, m_catalogDb, m_catalogScopeEpoch,
        m_librarySync, m_libraryQuery));
}

void App::goToLogin(const std::string &initialMessage)
{
    m_stack.popToRoot();
    m_stack.push(std::make_unique<LoginScreen>(
        m_serverUrl, m_serverInfo.serverName, m_deviceId, initialMessage));
}

void App::logout()
{
    printf("[App] Logging out\n");
    if (m_catalogDb) m_catalogScopeEpoch = m_catalogDb->deconfigureScope();
    if (m_downloadManager) {
        m_downloadManager->setLibraryServices({}, {});
        m_downloadManager->configure(Session{});
    }
    m_librarySync.reset();
    m_libraryQuery.reset();
    { std::lock_guard<std::mutex> lock(m_journalMutex); m_session.clear(); }
    Session::remove();
}
}
