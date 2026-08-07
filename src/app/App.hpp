#ifndef MIYOOFIN_APP_HPP
#define MIYOOFIN_APP_HPP

#include <SDL2/SDL.h>
#include <memory>
#include "ScreenStack.hpp"
#include "../input/InputManager.hpp"
#include "../net/JellyfinApi.hpp"

namespace miyoofin {

/// Owns the SDL lifecycle and the main event / render loop.
/// Manages the startup flow: splash -> connecting -> server entry -> home.
class App {
public:
    App();
    ~App();

    /// Initialise SDL, create window / renderer, push the startup screen.
    bool init();

    /// Run the main loop until exit is requested.
    int run();

private:
    SDL_Window     *m_window;
    SDL_Renderer   *m_renderer;
    SDL_Surface    *m_fb;         // 640x480 software framebuffer
    SDL_Texture    *m_fbTex;      // streaming texture uploaded from m_fb

    ScreenStack     m_stack;
    InputManager    m_input;

    bool            m_running;
    Uint32          m_lastTick;

    // Startup flow state
    std::string     m_serverUrl;   // saved server URL
    ServerInfo      m_serverInfo;  // connected server info

    /// Load saved server URL from server.txt.
    void loadSavedUrl();

    /// Transition to the home screen.
    void goToHome();

    // Prevent copy
    App(const App&) = delete;
    App& operator=(const App&) = delete;
};

} // namespace miyoofin

#endif // MIYOOFIN_APP_HPP