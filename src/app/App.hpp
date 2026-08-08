#ifndef MIYOOFIN_APP_HPP
#define MIYOOFIN_APP_HPP

#include <SDL2/SDL.h>
#include <memory>
#include "ScreenStack.hpp"
#include "../input/InputManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/Session.hpp"

namespace miyoofin {

/// Owns the SDL lifecycle and the main event / render loop.
/// Manages the startup flow:
///   splash -> connecting -> login (or auth-check) -> home.
class App {
public:
    App();
    ~App();

    /// Initialise SDL, create window / renderer, push the startup screen.
    bool init();

    /// Run the main loop until exit is requested.
    int run();

    /// Request a clean exit for playback handoff.
    /// Called by screens when a playback-request.txt has been written.
    /// Sets the playback-requested flag and terminates the main loop.
    void requestPlaybackExit();

    /// True if the app is exiting for a playback request (not user quit).
    bool playbackRequested() const { return m_playbackRequested; }

private:
    SDL_Window     *m_window;
    SDL_Renderer   *m_renderer;
    SDL_Surface    *m_fb;         // 640x480 software framebuffer
    SDL_Texture    *m_fbTex;      // streaming texture uploaded from m_fb

    ScreenStack     m_stack;
    InputManager    m_input;

    bool            m_running;
    bool            m_playbackRequested = false;
    Uint32          m_lastTick;

    // Startup flow state
    std::string     m_serverUrl;   // saved server URL
    ServerInfo      m_serverInfo;  // connected server info
    std::string     m_deviceId;    // persistent device identifier
    Session         m_session;     // saved session (token + user)

    /// Load saved server URL from server.txt.
    void loadSavedUrl();

    /// Load a saved session (if any) from session.txt.
    void loadSavedSession();

    /// Transition to the home screen.
    void goToHome();

    /// Transition to the login screen (pop stack to root first).
    void goToLogin(const std::string &initialMessage = {});

    /// Discard the current session (logout).
    void logout();

    // Prevent copy
    App(const App&) = delete;
    App& operator=(const App&) = delete;
};

} // namespace miyoofin

#endif // MIYOOFIN_APP_HPP