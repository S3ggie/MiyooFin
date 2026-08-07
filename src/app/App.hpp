#ifndef MIYOOFIN_APP_HPP
#define MIYOOFIN_APP_HPP

#include <SDL2/SDL.h>
#include <memory>
#include "ScreenStack.hpp"
#include "../input/InputManager.hpp"

namespace miyoofin {

/// Owns the SDL lifecycle and the main event / render loop.
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

    // Prevent copy
    App(const App&) = delete;
    App& operator=(const App&) = delete;
};

} // namespace miyoofin

#endif // MIYOOFIN_APP_HPP