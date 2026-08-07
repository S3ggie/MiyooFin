#include "App.hpp"
#include "../ui/Theme.hpp"
#include "../ui/screens/StartupScreen.hpp"
#include "../ui/screens/HomeScreen.hpp"
#include "../ui/screens/ServerEntryScreen.hpp"
#include "../ui/screens/ConnectScreen.hpp"
#include "miyoofin/version.hpp"
#include <curl/curl.h>
#include <cstdio>
#include <cstring>

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

void App::goToHome()
{
    if (m_stack.size() > 1) {
        m_stack.pop();
    }
    m_stack.push(std::make_unique<HomeScreen>());
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

        // --- Startup flow transitions ---
        Screen *top = m_stack.top();
        if (top) {
            if (auto *conn = dynamic_cast<ConnectScreen *>(top)) {
                if (conn->finished()) {
                    if (conn->connected()) {
                        m_serverUrl = conn->serverUrl();
                        m_serverInfo = conn->serverInfo();
                        printf("[App] ConnectScreen success -> Home\n");
                        goToHome();
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
                    printf("[App] ServerEntryScreen success -> Home\n");
                    goToHome();
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
