#ifndef MIYOOFIN_AUTH_CHECK_SCREEN_HPP
#define MIYOOFIN_AUTH_CHECK_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../net/Session.hpp"
#include <string>
#include <atomic>
#include <thread>

namespace miyoofin {

/// Shown at startup when a saved session exists.
/// Validates the saved access token against the server.
/// On success: shows a brief welcome then finished(ok=true).
/// On failure: sets finished(ok=false) so the App can route to login.
class AuthCheckScreen : public Screen {
public:
    explicit AuthCheckScreen(const Session &session);
    ~AuthCheckScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

    bool ok() const { return m_ok; }
    bool finished() const { return m_finished; }
    const std::string &errorMessage() const { return m_error; }
    const std::string &userName() const { return m_userName; }

private:
    Session m_session;
    std::string m_message;
    std::string m_error;
    std::string m_userName;
    bool m_ok = false;
    bool m_finished = false;
    Uint32 m_welcomeTimer = 0;

    std::thread m_checkThread;
    std::atomic<bool> m_checkDone{false};
    std::atomic<bool> m_checkSuccess{false};
    std::string m_checkError;

    void startCheck();
    void finishCheck();
};

} // namespace miyoofin

#endif // MIYOOFIN_AUTH_CHECK_SCREEN_HPP