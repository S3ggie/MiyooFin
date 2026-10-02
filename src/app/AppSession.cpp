#include "App.hpp"
#include "../ui/screens/HomeScreen.hpp"
#include "../ui/screens/MusicScreen.hpp"
#include "../ui/Design.hpp"
#include "../music/MusicTracks.hpp"
#include <unistd.h>
#include "../ui/screens/LoginScreen.hpp"
#include "../net/RouteRequest.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
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
        TokenValidation result = TokenValidation::Unavailable;
        RouteRequest(session).run(
            [&](const std::string& base) {
                result = JellyfinApi::validateTokenStatus(base, session.accessToken, session.userId,
                                                          session.deviceId, error);
                return result == TokenValidation::Valid;
            },
            error);
        if (result == TokenValidation::Valid && session.serverId.empty()) {
            ServerInfo info;
            std::string systemInfoError;
            if (RouteRequest(session).run(
                    [&](const std::string& base) {
                        return JellyfinApi::getSystemInfo(base, info, systemInfoError);
                    },
                    systemInfoError)) {
                m_savedSessionServerId = info.serverId;
            }
        }
        m_savedValidation = result == TokenValidation::Unauthorized
                                ? 2
                                : (result == TokenValidation::Valid ? 3 : 1);
    });
}

void App::finishSavedSessionValidation()
{
    if (!m_savedFastPath || m_savedValidation.load() == 0)
        return;
    if (m_savedValidationThread.joinable())
        m_savedValidationThread.join();
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
    FILE* f = fopen("server.txt", "r");
    if (!f)
        return;
    char buf[512];
    if (fgets(buf, sizeof(buf), f)) {
        buf[511] = '\0';
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
            buf[--len] = '\0';
        }
        m_serverUrl = buf;
    }
    fclose(f);
}

void App::configureCatalogScopeForSession()
{
    if (m_catalogDb && m_session.valid()) {
        if (m_libraryCoordinator) {
            m_libraryCoordinator->stop();
        }
        if (m_downloadManager)
            m_downloadManager->setLibraryServices({}, {});
        m_libraryCoordinator.reset();
        uiDiagnostics().log("[App] startup stage=catalog_scope_start");
        uiDiagnostics().log("[App] catalog scope request identity=valid");
        m_catalogScopeEpoch = m_catalogDb->configureScope(m_session.serverUrl, m_session.userId);
        m_libraryCoordinator = std::make_shared<library::LibraryCoordinator>(m_session, m_catalogDb,
                                                                             m_catalogScopeEpoch);
        // Home's cold-start startup/full-population sequence begins as soon as
        // this session's Home screen runs.  Reserve that precedence before the
        // live worker starts so a live event cannot claim the serialized slot
        // ahead of startup.  Home's controller captures the returned owner token
        // and releases the sequence if Home is torn down before it completes; a
        // stale controller from a previous generation holds a different token
        // and cannot clear this owner's reservation/handoff.
        // Only the video mode runs that sequence; in music mode live work simply proceeds.
        if (m_mode == AppMode::Video)
            m_libraryCoordinator->reserveStartupSequence();
        m_libraryCoordinator->start();
        if (m_downloadManager)
            m_downloadManager->setLibraryServices(m_libraryCoordinator->query(),
                                                  m_libraryCoordinator);
        uiDiagnostics().log("[App] startup stage=catalog_scope_requested");
    } else if (m_catalogDb) {
        uiDiagnostics().log("[App] catalog scope request identity=invalid");
        if (m_libraryCoordinator) {
            m_libraryCoordinator->stop();
        }
        if (m_downloadManager)
            m_downloadManager->setLibraryServices({}, {});
        m_catalogScopeEpoch = m_catalogDb->deconfigureScope();
        m_libraryCoordinator.reset();
    }
}

void App::loadSavedSession()
{
    m_session = Session::load();
    if (m_session.valid()) {
        uiDiagnostics().log("[App] saved session valid=1");
        printf("[App] Loaded saved session for user '%s'\n", m_session.userName.c_str());
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
    if (m_mode == AppMode::Music) {
        ensureMusicPlayer();
        std::atomic_store(&m_musicSession, std::make_shared<Session>(m_session));
        m_stack.push(std::make_unique<MusicScreen>(m_session, m_downloadManager, m_music.get(),
                                                   &m_musicSettings));
        return;
    }
    m_stack.push(std::make_unique<HomeScreen>(
        m_session, m_downloadManager,
        m_libraryCoordinator ? m_libraryCoordinator->query() : nullptr, m_libraryCoordinator));
}

void App::ensureMusicPlayer()
{
    if (m_music)
        return;
    music::PlayerOptions options;
    char cwd[1024];
    const std::string dir = getcwd(cwd, sizeof(cwd)) ? std::string(cwd) : std::string(".");
    options.enginePath = dir + "/miyoofin-audio";
    options.engineLog = dir + "/music-engine.log";
    // The audio path that ffplay proved on this device: OSS emulation through padsp, and
    // none of the SDL driver overrides the launcher exports for the SDL2 UI.
    options.engineEnv = {"LD_PRELOAD=/mnt/SDCARD/miyoo/lib/libpadsp.so"};
    options.engineUnsetEnv = {"SDL_AUDIODRIVER", "SDL_VIDEODRIVER", "LD_PRELOAD"};
    m_musicSettings.load("music-settings.txt");

    music::PlayerHooks hooks;
    hooks.resolve = [this](const music::Track& track, const std::atomic<bool>& cancelled) {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        music::ResolvedTrack out;
        if (!session) {
            out.error = "Not signed in";
            return out;
        }
        return music::resolveTrack(
            music::makeServerSource(*session, "music-cache/stream",
                                    {music::MusicSettings::sanitize(m_musicSettings.streamKbps)}),
            track, cancelled);
    };
    hooks.report = [this](music::ReportKind kind, const music::Track& track, std::int64_t ticks,
                          bool paused, const std::string& playSessionId) {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        if (!session)
            return;
        std::string error;
        music::onRoute(*session, error, [&](const music::Connection& c) {
            return music::reportPlayback(c, kind, track.id, ticks, paused, playSessionId, error);
        });
    };
    hooks.setAwake = [](bool awake) {
        // Onion's idle sleep checks this file, exactly as it does during video playback.
        if (awake) {
            if (FILE* f = std::fopen("/tmp/stay_awake", "w"))
                std::fclose(f);
        } else {
            std::remove("/tmp/stay_awake");
        }
    };
    m_music = std::make_unique<music::MusicPlayer>(std::move(options), std::move(hooks));
}

void App::switchMode(AppMode mode)
{
    if (mode == m_mode)
        return;
    printf("[App] Switching to %s mode\n", mode == AppMode::Music ? "music" : "video");
    if (auto* home = dynamic_cast<HomeScreen*>(m_stack.top()))
        home->cancelAsyncWork();
    if (mode == AppMode::Video && m_music)
        m_music->stop(); // playback does not follow you into the video app
    m_mode = mode;
    saveAppMode(m_mode);
    design::usePalette(m_mode == AppMode::Music);
    if (m_mode == AppMode::Video)
        configureCatalogScopeForSession(); // fresh coordinator with Home's startup reservation
    goToHome();
}

void App::goToLogin(const std::string& initialMessage)
{
    m_stack.popToRoot();
    m_stack.push(std::make_unique<LoginScreen>(m_serverUrl, m_serverInfo.serverName, m_deviceId,
                                               initialMessage));
}

void App::logout()
{
    printf("[App] Logging out\n");
    if (m_music)
        m_music->stop();
    if (m_libraryCoordinator) {
        m_libraryCoordinator->stop();
    }
    if (m_downloadManager) {
        m_downloadManager->setLibraryServices({}, {});
        m_downloadManager->configure(Session{});
    }
    if (m_catalogDb)
        m_catalogScopeEpoch = m_catalogDb->deconfigureScope();
    m_libraryCoordinator.reset();
    {
        std::lock_guard<std::mutex> lock(m_journalMutex);
        m_session.clear();
    }
    Session::remove();
}
}
