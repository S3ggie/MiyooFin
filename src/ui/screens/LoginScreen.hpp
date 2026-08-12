#ifndef MIYOOFIN_LOGIN_SCREEN_HPP
#define MIYOOFIN_LOGIN_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../OnScreenKeyboard.hpp"
#include <string>
#include <atomic>
#include <thread>

namespace miyoofin {

/// Screen for entering a Jellyfin username and password using the
/// on-screen keyboard. The password is masked while typing.
/// On success, fills the session data and sets finished().
/// On failure, shows a useful error message and lets the user retry.
class LoginScreen : public Screen {
public:
    LoginScreen(const std::string &serverUrl,
                const std::string &serverName,
                const std::string &deviceId,
                const std::string &initialMessage = {});
    ~LoginScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

    /// True when the user successfully authenticated.
    bool finished() const { return m_finished; }

    /// True if authentication succeeded (implies finished()).
    bool success() const { return m_success; }

    /// The populated session data (valid only when success()).
    const AuthResult &result() const { return m_result; }

    /// The server URL used for this login.
    const std::string &serverUrl() const { return m_serverUrl; }

    // Keyboard state accessors support deterministic host tests.
    bool capsEnabled() const { return m_keyboard.capsEnabled(); }
    const std::string &username() const { return m_username; }
    const std::string &password() const { return m_password; }
    bool keyboardSelectionValid() const { return m_keyboard.selectionValid(); }

private:
    OnScreenKeyboard m_keyboard;

    // Fields
    std::string m_username;
    std::string m_password;
    int m_activeField = 0;   // 0 = username, 1 = password
    bool m_inFields = true;  // true when focus is on the field row

    std::string m_serverUrl;
    std::string m_serverName;
    std::string m_deviceId;
    std::string m_message;

    // Login attempt state
    bool m_connecting = false;
    bool m_finished = false;
    bool m_success = false;
    AuthResult m_result;
    std::thread m_loginThread;
    std::atomic<bool> m_loginDone{false};
    std::atomic<bool> m_loginSuccess{false};
    std::string m_loginError;
    AuthResult m_loginResult;

    void submitLogin();
    void finishLogin();
    std::string &activeText();

    // Drawing helpers
    void drawTitle(SDL_Surface *fb);
    void drawField(SDL_Surface *fb, int y, const char *label,
                   const std::string &display, bool selected, bool masked);
    void drawInputFields(SDL_Surface *fb);
    void drawStatus(SDL_Surface *fb);
    void drawHints(SDL_Surface *fb);
};

} // namespace miyoofin

#endif // MIYOOFIN_LOGIN_SCREEN_HPP
