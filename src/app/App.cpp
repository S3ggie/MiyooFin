#include "App.hpp"
#include "../ui/Theme.hpp"
#include "../ui/screens/StartupScreen.hpp"
#include "../ui/screens/InputDiagnosticsScreen.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>

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
}

bool App::init()
{
    printf("[App] %s %s on %s\n", APP_NAME, VERSION_STR, DEVICE_NAME);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) < 0) {
        fprintf(stderr, "[App] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    // Determine actual display dimensions (fallback to 640x480)
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

    // Create the single software framebuffer (640x480 RGBA32)
    m_fb = SDL_CreateRGBSurfaceWithFormat(
        0, SCREEN_W, SCREEN_H, 32, SDL_PIXELFORMAT_RGBA32
    );
    if (!m_fb) {
        fprintf(stderr, "[App] Failed to create framebuffer surface: %s\n", SDL_GetError());
        return false;
    }

    // Create the single streaming texture for upload
    m_fbTex = SDL_CreateTexture(
        m_renderer, SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H
    );
    if (!m_fbTex) {
        fprintf(stderr, "[App] Failed to create streaming texture: %s\n", SDL_GetError());
        return false;
    }

    // Push the startup screen, then immediately push diagnostics
    m_stack.push(std::make_unique<StartupScreen>());

    // After a brief display of the startup screen, we push diagnostics.
    // For now push it immediately so Checkpoint A is interactive.
    m_stack.push(std::make_unique<InputDiagnosticsScreen>(&m_input));

    m_running = true;
    m_lastTick = SDL_GetTicks();
    printf("[App] Initialisation complete\n");
    return true;
}

int App::run()
{
    while (m_running) {
        // --- Frame timing ---
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
            // Forward actions to the active screen
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

        // --- Render ---
        // Clear the software framebuffer to the background colour
        SDL_FillRect(m_fb, nullptr,
                     SDL_MapRGBA(m_fb->format,
                                 Theme::BG_R, Theme::BG_G,
                                 Theme::BG_B, Theme::BG_A));

        if (active) {
            active->render(m_fb);
        }

        // Upload software surface to streaming texture
        SDL_UpdateTexture(m_fbTex, nullptr, m_fb->pixels, m_fb->pitch);

        // Present
        SDL_RenderClear(m_renderer);
        SDL_RenderCopy(m_renderer, m_fbTex, nullptr, nullptr);
        SDL_RenderPresent(m_renderer);
    }

    printf("[App] Exiting cleanly\n");
    return 0;
}

} // namespace miyoofin