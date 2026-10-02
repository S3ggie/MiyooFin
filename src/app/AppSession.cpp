#include "App.hpp"
#include "../ui/screens/HomeScreen.hpp"
#include "../ui/screens/MusicScreen.hpp"
#include "../ui/Design.hpp"
#include "../music/MusicTracks.hpp"
#include "../music/MusicDownloads.hpp"
#include "../music/PlaysJournal.hpp"
#include <ctime>
#include <sys/statvfs.h>
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
    loadRouteMemory();
    m_session = Session::load();
    m_session.makeRoutesExplicit(); // older session files: derive the two addresses once
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
        m_stack.push(std::make_unique<MusicScreen>(m_session, m_music.get(), &m_musicSettings,
                                                   m_musicDownloads.get()));
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

    music::MusicDownloads::Hooks downloadHooks;
    downloadHooks.fetch = [this](const std::string& trackId, const std::string& dest,
                                 std::string& error, const std::atomic<bool>& cancelled) {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        if (!session) {
            error = "Not signed in";
            return false;
        }
        const music::AudioQuality quality{
            music::MusicSettings::sanitize(m_musicSettings.downloadKbps)};
        return music::onRoute(*session, error, [&](const music::Connection& c) {
            return music::downloadTrack(c, trackId, quality, dest, error, &cancelled);
        });
    };
    downloadHooks.offline = [this] {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        return !session || session->manualOfflineMode;
    };
    downloadHooks.freeBytes = [] {
        struct statvfs vfs;
        if (statvfs("music-downloads", &vfs) != 0 && statvfs(".", &vfs) != 0)
            return std::uint64_t{0};
        return static_cast<std::uint64_t>(vfs.f_bavail) * vfs.f_frsize;
    };
    m_musicDownloads =
        std::make_unique<music::MusicDownloads>("music-downloads", std::move(downloadHooks));

    music::PlayerHooks hooks;
    hooks.resolve = [this](const music::Track& track, const std::atomic<bool>& cancelled) {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        music::ResolvedTrack out;
        if (!session) {
            out.error = "Not signed in";
            return out;
        }
        music::TrackSourceConfig source =
            music::makeServerSource(*session, "music-cache/stream",
                                    {music::MusicSettings::sanitize(m_musicSettings.streamKbps)});
        // Offline copies win over everything else (and work with no network at all).
        source.downloadedPath = [this](const std::string& id) {
            return m_musicDownloads ? m_musicDownloads->pathFor(id) : std::string();
        };
        return music::resolveTrack(source, track, cancelled);
    };
    hooks.report = [this](music::ReportKind kind, const music::Track& track, std::int64_t ticks,
                          bool paused, const std::string& playSessionId) {
        const std::shared_ptr<Session> session = std::atomic_load(&m_musicSession);
        if (!session)
            return;
        static music::PlaysJournal journal("music-plays.journal");
        std::string error;
        const bool reported = !session->manualOfflineMode &&
                              music::onRoute(*session, error, [&](const music::Connection& c) {
                                  return music::reportPlayback(c, kind, track.id, ticks, paused,
                                                               playSessionId, error);
                              });
        // A play that could not be reported still counts: it is sent later with its time.
        if (kind == music::ReportKind::Stopped && !reported &&
            music::countsAsPlayed(static_cast<double>(ticks) / 1e7, track.durationSeconds()))
            journal.add(track.id, static_cast<std::int64_t>(std::time(nullptr)));
        if (reported) {
            journal.flush([&](const music::PlaysJournal::Entry& e) {
                bool gone = false;
                std::string err;
                const bool ok = music::onRoute(*session, err, [&](const music::Connection& c) {
                    return music::markPlayed(
                        c, e.trackId, music::PlaysJournal::isoTime(e.epochSeconds), gone, err);
                });
                return ok || gone;
            });
        }
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
    hooks.saveQueue = [](const std::string& text) {
        if (text.empty()) {
            std::remove("music-queue.txt");
            return;
        }
        if (FILE* f = std::fopen("music-queue.txt.tmp", "w")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            std::rename("music-queue.txt.tmp", "music-queue.txt");
        }
    };
    m_music = std::make_unique<music::MusicPlayer>(std::move(options), std::move(hooks));
    if (FILE* f = std::fopen("music-queue.txt", "r")) {
        std::string text;
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        std::fclose(f);
        music::SavedQueue saved;
        if (music::parseQueue(text, saved))
            m_music->restoreQueue(saved);
    }
}

void App::switchMode(AppMode mode)
{
    if (mode == m_mode)
        return;
    printf("[App] Switching to %s mode\n", mode == AppMode::Music ? "music" : "video");
    if (auto* home = dynamic_cast<HomeScreen*>(m_stack.top()))
        home->cancelAsyncWork();
    if (mode == AppMode::Video && m_music)
        m_music->shutdown(1500); // playback does not follow you into the video app
    // The video Settings tab saves offline mode into session.txt from its own copy of the
    // session; pick that up so both modes (and the new screen) agree on it.
    {
        const Session saved = Session::load();
        if (saved.valid()) {
            std::lock_guard<std::mutex> lock(m_journalMutex);
            m_session.manualOfflineMode = saved.manualOfflineMode;
        }
    }
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

// Signing out (or the server dropping the sign-in) must not lose the addresses the user set up:
// they are kept in routes-memory.txt and come back when the same server is signed in to again.
void App::saveRouteMemory() const
{
    if (m_routeMemory.identity.empty())
        return;
    if (FILE* f = std::fopen("routes-memory.txt.tmp", "w")) {
        std::fprintf(f, "identity=%s\nlan=%s\npublic=%s\n", m_routeMemory.identity.c_str(),
                     m_routeMemory.lan.c_str(), m_routeMemory.pub.c_str());
        std::fclose(f);
        std::rename("routes-memory.txt.tmp", "routes-memory.txt");
    }
}

void App::loadRouteMemory()
{
    FILE* f = std::fopen("routes-memory.txt", "r");
    if (!f)
        return;
    char line[1100];
    RouteMemory memory;
    while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
            l.pop_back();
        if (l.compare(0, 9, "identity=") == 0)
            memory.identity = l.substr(9);
        else if (l.compare(0, 4, "lan=") == 0)
            memory.lan = l.substr(4);
        else if (l.compare(0, 7, "public=") == 0)
            memory.pub = l.substr(7);
    }
    std::fclose(f);
    m_routeMemory = memory;
}

void App::logout()
{
    printf("[App] Logging out\n");
    {
        const Session::Routes routes = m_session.routes();
        m_routeMemory = {m_session.serverUrl, routes.lan, routes.pub};
        saveRouteMemory();
    }
    if (m_music)
        m_music->stop();
    std::atomic_store(&m_musicSession, std::shared_ptr<Session>()); // music workers: signed out
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
