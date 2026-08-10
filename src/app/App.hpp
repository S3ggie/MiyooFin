#ifndef MIYOOFIN_APP_HPP
#define MIYOOFIN_APP_HPP

#include <SDL2/SDL.h>
#include <memory>
#include <atomic>
#include <thread>
#include "ScreenStack.hpp"
#include "../input/InputManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/Session.hpp"

namespace miyoofin {

/// Owns the SDL lifecycle and the main event / render loop.
/// Manages the startup flow:
///   splash -> connecting -> login (or auth-check) -> home.
///
/// B5f3a: supports in-process external playback handoff — the app
/// suspends SDL/video/input, spawns a child FFplay process, waits for
/// it, then reinitializes SDL and resumes the same screen state.
class App {
public:
    App();
    ~App();

    /// Initialise SDL, create window / renderer, push the startup screen.
    bool init();

    /// Run the main loop until exit is requested.
    int run();

    /// Suspend SDL video/input/joystick subsystems.
    /// Releases hardware resources (framebuffer, window, renderer,
    /// joystick) so an external playback process (FFplay) can acquire
    /// them.  Logical app state (ScreenStack, screen objects, artwork
    /// caches) is untouched.
    /// @return true on success.
    bool suspendPlatform();

    /// Reinitialize SDL video/input/joystick subsystems after
    /// external playback completes.  Recreates window, renderer,
    /// framebuffer surface, streaming texture, and reopens the
    /// joystick.  Clears any stale SDL events.
    /// @return true on success.
    bool resumePlatform();

private:
    SDL_Window     *m_window;
    SDL_Renderer   *m_renderer;
    SDL_Surface    *m_fb;         // 640x480 software framebuffer
    SDL_Texture    *m_fbTex;      // streaming texture uploaded from m_fb

    ScreenStack     m_stack;
    InputManager    m_input;

    bool            m_running;
    Uint32          m_lastTick;

    // Short visual acknowledgement shown before the external player takes
    // ownership of the framebuffer.
    bool            m_playbackStarting;
    Uint32          m_playbackStartingTick;

    // Startup flow state
    std::string     m_serverUrl;   // saved server URL
    ServerInfo      m_serverInfo;  // connected server info
    std::string     m_deviceId;    // persistent device identifier
    Session         m_session;     // saved session (token + user)
    std::thread     m_savedValidationThread;
    std::atomic<int> m_savedValidation{0}; // 0 pending, 1 valid/unavailable, 2 unauthorized
    bool            m_savedFastPath = false;

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
    void startSavedSessionValidation();
    void finishSavedSessionValidation();

    /// Handle an external playback request: suspend platform, fork
    /// playback_runner.sh, wait for child, resume platform.
    void handleExternalPlayback();

    /// Draw the playback-starting message over the current screen.
    void drawPlaybackStartingOverlay(Uint32 elapsedMs);

    // Prevent copy
    App(const App&) = delete;
    App& operator=(const App&) = delete;
};

} // namespace miyoofin

#endif // MIYOOFIN_APP_HPP
