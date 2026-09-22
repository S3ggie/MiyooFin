#ifndef MIYOOFIN_CONNECT_SCREEN_HPP
#define MIYOOFIN_CONNECT_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../net/JellyfinApi.hpp"
#include <string>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace miyoofin {

/// Screen shown at startup when a saved server URL exists.
/// Tries to connect with brief retries, showing "Connecting...".
/// On success: shows server info briefly, then sets finished().
/// On failure: sets finished() + failed() with error message.
/// The App manages the transition after finished().
class ConnectScreen : public Screen
{
  public:
    explicit ConnectScreen(const std::string& savedUrl);
#ifdef MIYOOFIN_TEST_BUILD
    using ConnectionAttempt = std::function<bool(const std::string&, ServerInfo&, std::string&,
                                                 const std::atomic<bool>*)>;
    ConnectScreen(const std::string& savedUrl, ConnectionAttempt attempt);
#endif
    ~ConnectScreen() override;

    void enter() override;
    void leave() override;
    bool deferDestruction() const override
    {
        return true;
    }
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface* fb) override;

    bool connected() const
    {
        return m_connected;
    }
    bool failed() const
    {
        return m_failed;
    }
    bool finished() const
    {
        return m_finished;
    }
    const ServerInfo& serverInfo() const
    {
        return m_serverInfo;
    }
    const std::string& serverUrl() const
    {
        return m_savedUrl;
    }
    const std::string& errorMessage() const
    {
        return m_error;
    }

  private:
    std::string m_savedUrl;
    std::string m_message;
    std::string m_error;
    ServerInfo m_serverInfo;
    bool m_connected = false;
    bool m_failed = false;
    bool m_finished = false;
    int m_retriesLeft = 3;
    Uint32 m_retryTimer = 0;
    Uint32 m_infoTimer = 0; // countdown when showing connected info

    // Connection thread
    std::thread m_connectThread;
    std::shared_ptr<std::atomic<bool>> m_connectCancellation;
    std::atomic<bool> m_connectDone{false};
    std::atomic<bool> m_connectSuccess{false};
    std::string m_connectError;
    ServerInfo m_connectResult;
#ifdef MIYOOFIN_TEST_BUILD
    ConnectionAttempt m_connectionAttempt;
#endif

    void startConnection();
    void finishConnection();
    void stopConnection();
};

} // namespace miyoofin

#endif // MIYOOFIN_CONNECT_SCREEN_HPP
