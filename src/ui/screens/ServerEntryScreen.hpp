#ifndef MIYOOFIN_SERVER_ENTRY_SCREEN_HPP
#define MIYOOFIN_SERVER_ENTRY_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../OnScreenKeyboard.hpp"
#include <string>
#include <atomic>
#include <thread>

namespace miyoofin {

/// Screen for entering a Jellyfin server URL using an on-screen keyboard.
/// Supports D-pad navigation, A to type, B to delete, START to confirm.
/// After a successful connection, saves the URL and pops itself.
/// The caller (App) should check getResult() to know if it succeeded.
class ServerEntryScreen : public Screen {
public:
    ServerEntryScreen();
    ServerEntryScreen(const std::string &initialUrl, const std::string &errorMsg);
    ServerEntryScreen(const std::string &initialUrl, const std::string &errorMsg,
                      const std::string &expectedServerId, bool localAddressEntry,
                      bool publicAddressEntry=false);
    ~ServerEntryScreen() override;

    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

    /// Returns true if the user successfully connected to a server.
    bool connected() const { return m_connected; }

    /// Returns true when the screen is done (connected info shown long enough).
    bool finished() const { return m_finished; }

    /// Returns the saved server URL after successful connection.
    const std::string &serverUrl() const { return m_serverUrl; }

    /// Returns the server info after successful connection.
    const ServerInfo &serverInfo() const { return m_serverInfo; }
    bool localAddressEntry() const { return m_localAddressEntry; }
    bool publicAddressEntry() const { return m_publicAddressEntry; }
    bool addressEntryCancelled() const { return m_addressEntryCancelled; }

    // Keyboard state accessors support deterministic host tests.
    bool capsEnabled() const { return m_keyboard.capsEnabled(); }
    const std::string &entryText() const { return m_url; }
    bool keyboardSelectionValid() const { return m_keyboard.selectionValid(); }

private:
    OnScreenKeyboard m_keyboard;

    std::string m_url;           // current URL being typed
    std::string m_message;       // status or error message
    bool m_connected = false;
    bool m_connecting = false;
    ServerInfo m_serverInfo;
    std::string m_serverUrl;
    std::string m_expectedServerId;
    bool m_localAddressEntry = false;
    bool m_publicAddressEntry = false;
    bool m_addressEntryCancelled = false;

    // Connection attempt thread
    std::thread m_connectThread;
    std::atomic<bool> m_connectDone{false};
    std::atomic<bool> m_connectSuccess{false};
    std::string m_connectError;
    ServerInfo m_connectResult;
    bool m_finished = false;
    Uint32 m_infoTimer = 0;

    void startConnection();
    void finishConnection();
    void cancelAddressEntry();

    // Drawing helpers
    void drawInputField(SDL_Surface *fb);
    void drawStatus(SDL_Surface *fb);
};

} // namespace miyoofin

#endif // MIYOOFIN_SERVER_ENTRY_SCREEN_HPP
