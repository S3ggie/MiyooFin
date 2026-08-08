#include "App.hpp"
#include "../ui/Theme.hpp"
#include "../ui/screens/StartupScreen.hpp"
#include "../ui/screens/HomeScreen.hpp"
#include "../ui/screens/ServerEntryScreen.hpp"
#include "../ui/screens/ConnectScreen.hpp"
#include "../ui/screens/LoginScreen.hpp"
#include "../ui/screens/AuthCheckScreen.hpp"
#include "../net/DeviceIdentity.hpp"
#include "miyoofin/version.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <cerrno>
#include <string>

namespace miyoofin {

App::App()
    : m_window(nullptr)
    , m_renderer(nullptr)
    , m_fb(nullptr)
    , m_fbTex(nullptr)
    , m_running(false)
    , m_lastTick(0)
{
}

App::~App()
{
    if (m_fbTex)  SDL_DestroyTexture(m_fbTex);
    if (m_fb)     SDL_FreeSurface(m_fb);
    if (m_renderer) SDL_DestroyRenderer(m_renderer);
    if (m_window) SDL_DestroyWindow(m_window);
    SDL_Quit();
    curl_global_cleanup();
}

bool App::init()
{
    printf("[App] %s %s on %s\n", APP_NAME, VERSION_STR, DEVICE_NAME);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[App] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    int displayW = SCREEN_W;
    int displayH = SCREEN_H;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        displayW = dm.w;
        displayH = dm.h;
        printf("[App] Display mode: %dx%d @ %d Hz\n", displayW, displayH, dm.refresh_rate);
    } else {
        printf("[App] Using fallback dimensions: %dx%d\n", displayW, displayH);
    }

    m_window = SDL_CreateWindow(
        APP_NAME,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        displayW, displayH,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!m_window) {
        fprintf(stderr, "[App] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    m_renderer = SDL_CreateRenderer(
        m_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!m_renderer) {
        fprintf(stderr, "[App] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);

    m_fb = SDL_CreateRGBSurfaceWithFormat(
        0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_RGBA32
    );
    if (!m_fb) {
        fprintf(stderr, "[App] Failed to create framebuffer: %s\n", SDL_GetError());
        return false;
    }

    m_fbTex = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H
    );
    if (!m_fbTex) {
        fprintf(stderr, "[App] Failed to create streaming texture: %s\n", SDL_GetError());
        return false;
    }

    // -- Startup flow --
    m_stack.push(std::make_unique<StartupScreen>());

    loadSavedUrl();
    loadSavedSession();
    m_deviceId = DeviceIdentity::loadOrCreate();
    printf("[App] Device ID: %s\n", m_deviceId.c_str());

    if (!m_serverUrl.empty()) {
        printf("[App] Saved server URL: %s\n", m_serverUrl.c_str());
        m_stack.push(std::make_unique<ConnectScreen>(m_serverUrl));
    } else {
        printf("[App] No saved server URL\n");
        m_stack.push(std::make_unique<ServerEntryScreen>());
    }

    m_running = true;
    m_lastTick = SDL_GetTicks();
    printf("[App] Initialisation complete\n");
    return true;
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

void App::loadSavedSession()
{
    m_session = Session::load();
    if (m_session.valid()) {
        printf("[App] Loaded saved session for user '%s'\n",
               m_session.userName.c_str());
        // Prefer the session's server URL if a server URL is not yet known
        if (m_serverUrl.empty() && !m_session.serverUrl.empty()) {
            m_serverUrl = m_session.serverUrl;
        }
    } else {
        printf("[App] No valid saved session\n");
    }
}

void App::goToHome()
{
    if (m_stack.size() > 1) {
        m_stack.pop();
    }
    m_stack.push(std::make_unique<HomeScreen>(m_session));
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
    m_session.clear();
    Session::remove();
}

bool App::suspendPlatform()
{
    printf("[App] Suspending platform resources\n");

    // Destroy SDL resources in reverse order of creation
    if (m_fbTex)    { SDL_DestroyTexture(m_fbTex);      m_fbTex = nullptr; }
    if (m_fb)       { SDL_FreeSurface(m_fb);             m_fb = nullptr; }
    if (m_renderer) { SDL_DestroyRenderer(m_renderer);   m_renderer = nullptr; }
    if (m_window)   { SDL_DestroyWindow(m_window);       m_window = nullptr; }

    // Suspend input (joystick will be closed by QuitSubSystem)
    m_input.suspend();

    // Shut down SDL subsystems — releases framebuffer/video hardware
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER);

    printf("[App] Platform suspended\n");
    return true;
}

bool App::resumePlatform()
{
    printf("[App] Resuming platform resources\n");

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[App] SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return false;
    }

    int displayW = SCREEN_W;
    int displayH = SCREEN_H;
    SDL_DisplayMode dm;
    if (SDL_GetDesktopDisplayMode(0, &dm) == 0) {
        displayW = dm.w;
        displayH = dm.h;
    }

    m_window = SDL_CreateWindow(
        APP_NAME,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        displayW, displayH,
        SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI
    );
    if (!m_window) {
        fprintf(stderr, "[App] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    m_renderer = SDL_CreateRenderer(
        m_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
    );
    if (!m_renderer) {
        fprintf(stderr, "[App] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_SetRenderDrawBlendMode(m_renderer, SDL_BLENDMODE_NONE);

    m_fb = SDL_CreateRGBSurfaceWithFormat(
        0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_RGBA32
    );
    if (!m_fb) {
        fprintf(stderr, "[App] Failed to create framebuffer: %s\n", SDL_GetError());
        return false;
    }

    m_fbTex = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H
    );
    if (!m_fbTex) {
        fprintf(stderr, "[App] Failed to create streaming texture: %s\n", SDL_GetError());
        return false;
    }

    // Reopen the joystick
    m_input.resume();

    // Clear any stale SDL events
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {}

    printf("[App] Platform resumed\n");
    return true;
}

void App::handleExternalPlayback()
{
    printf("[App] Starting external playback handoff\n");

    // 1. Suspend SDL/video/input
    if (!suspendPlatform()) {
        fprintf(stderr, "[App] Failed to suspend platform — aborting playback\n");
        return;
    }

    // 2. Locate the playback runner script relative to this binary
    std::string runnerPath;
    {
        char exePath[4096];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            std::string path(exePath);
            size_t lastSlash = path.rfind('/');
            if (lastSlash != std::string::npos) {
                runnerPath = path.substr(0, lastSlash + 1) + "playback_runner.sh";
            }
        }
    }
    if (runnerPath.empty()) {
        runnerPath = "./playback_runner.sh";
    }

    printf("[App] Playback runner: %s\n", runnerPath.c_str());

    // 3. Fork and exec the playback runner
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[App] fork() failed: %s\n", strerror(errno));
        resumePlatform();
        return;
    }

    if (pid == 0) {
        // Child process — exec the playback runner script
        execl("/bin/sh", "sh", runnerPath.c_str(), (char *)nullptr);
        // exec failed
        _exit(127);
    }

    // 4. Parent waits for child to finish
    printf("[App] Waiting for playback child (PID=%d)\n", pid);
    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        printf("[App] Playback child exited with status %d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("[App] Playback child killed by signal %d\n", WTERMSIG(status));
    }

    // 5. Resume SDL/video/input
    if (!resumePlatform()) {
        fprintf(stderr, "[App] Failed to resume platform — exiting\n");
        m_running = false;
        return;
    }

    printf("[App] External playback handoff complete, resuming UI\n");
}

int App::run()
{
    while (m_running) {
        Uint32 now = SDL_GetTicks();
        Uint32 dt = now - m_lastTick;
        m_lastTick = now;

        // --- Input ---
        std::vector<Action> actions = m_input.poll();
        for (Action a : actions) {
            if (a == Action::Exit) {
                m_running = false;
                break;
            }
            Screen *active = m_stack.top();
            if (active) {
                active->handleAction(a);
            }
        }

        // --- Update ---
        Screen *active = m_stack.top();
        if (active) {
            active->update(dt);
        }

        // --- Check if a screen requested external playback ---
        if (m_stack.pollExternalPlayback()) {
            printf("[App] External playback flagged by screen\n");
            handleExternalPlayback();
            // Reset timing so dt doesn't include playback duration
            m_lastTick = SDL_GetTicks();
        }

        // --- Startup flow transitions ---
        Screen *top = m_stack.top();
        if (top) {
            if (auto *conn = dynamic_cast<ConnectScreen *>(top)) {
                if (conn->finished()) {
                    if (conn->connected()) {
                        m_serverUrl = conn->serverUrl();
                        m_serverInfo = conn->serverInfo();
                        m_stack.pop();  // remove ConnectScreen

                        if (m_session.valid()) {
                            printf("[App] ConnectScreen success -> AuthCheck\n");
                            m_stack.push(
                                std::make_unique<AuthCheckScreen>(m_session));
                        } else {
                            printf("[App] ConnectScreen success -> Login\n");
                            m_stack.push(
                                std::make_unique<LoginScreen>(
                                    m_serverUrl, m_serverInfo.serverName,
                                    m_deviceId));
                        }
                    } else if (conn->failed()) {
                        printf("[App] ConnectScreen fail -> ServerEntry\n");
                        m_stack.pop();
                        std::string msg = "Could not reach " + m_serverUrl
                                        + ": " + conn->errorMessage();
                        m_stack.push(
                            std::make_unique<ServerEntryScreen>(m_serverUrl, msg));
                    }
                }
            }
            else if (auto *entry = dynamic_cast<ServerEntryScreen *>(top)) {
                if (entry->connected() && entry->finished()) {
                    m_serverUrl = entry->serverUrl();
                    m_serverInfo = entry->serverInfo();
                    printf("[App] ServerEntryScreen success -> Login\n");
                    m_stack.pop();
                    m_stack.push(
                        std::make_unique<LoginScreen>(
                            m_serverUrl, m_serverInfo.serverName, m_deviceId));
                }
            }
            else if (auto *authCheck = dynamic_cast<AuthCheckScreen *>(top)) {
                if (authCheck->finished()) {
                    if (authCheck->ok()) {
                        printf("[App] AuthCheckScreen valid -> Home\n");
                        m_stack.pop();
                        goToHome();
                    } else {
                        printf("[App] AuthCheckScreen invalid -> Login\n");
                        m_stack.pop();
                        m_stack.push(
                            std::make_unique<LoginScreen>(
                                m_serverUrl, m_serverInfo.serverName,
                                m_deviceId, authCheck->errorMessage()));
                    }
                }
            }
            else if (auto *login = dynamic_cast<LoginScreen *>(top)) {
                if (login->finished()) {
                    if (login->success()) {
                        // Save the session
                        printf("[App] LoginScreen success -> Home\n");
                        m_session.serverUrl   = m_serverUrl;
                        m_session.accessToken = login->result().accessToken;
                        m_session.userId      = login->result().userId;
                        m_session.userName    = login->result().userName;
                        m_session.deviceId    = m_deviceId;
                        m_session.save();

                        // Also save server URL for standalone use
                        FILE *sf = fopen("server.txt", "w");
                        if (sf) {
                            fprintf(sf, "%s\n", m_serverUrl.c_str());
                            fclose(sf);
                        }

                        m_stack.pop();
                        goToHome();
                    }
                    // Login failed — screen stays with error message
                }
            }
            else if (auto *home = dynamic_cast<HomeScreen *>(top)) {
                if (home->logoutRequested()) {
                    logout();
                    m_stack.popToRoot();
                    m_stack.push(
                        std::make_unique<LoginScreen>(
                            m_serverUrl, m_serverInfo.serverName, m_deviceId,
                            "Logged out successfully."));
                }
            }
        }

        // --- Render ---
        SDL_FillRect(m_fb, nullptr,
                     SDL_MapRGBA(m_fb->format,
                                 Theme::BG_R, Theme::BG_G,
                                 Theme::BG_B, Theme::BG_A));

        Screen *renderTop = m_stack.top();
        if (renderTop) {
            renderTop->render(m_fb);
        }

        SDL_UpdateTexture(m_fbTex, nullptr, m_fb->pixels, m_fb->pitch);

        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_fbTex, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);
    }

    printf("[App] Exiting cleanly\n");
    return 0;
}

} // namespace miyoofin
