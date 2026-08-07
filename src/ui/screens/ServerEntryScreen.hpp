#ifndef MIYOOFIN_SERVER_ENTRY_SCREEN_HPP
#define MIYOOFIN_SERVER_ENTRY_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../net/JellyfinApi.hpp"
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

private:
    // Keyboard layout
    struct Key {
        char ch;           // character to append (0 for special)
        const char *label; // display label
        int row, col;      // grid position
    };

    static constexpr int KEY_W = 32;   // key width in pixels
    static constexpr int KEY_H = 22;   // key height in pixels
    static constexpr int KEY_GAP = 2;  // gap between keys
    static constexpr int KEYBOARD_TOP = 100;  // Y offset for keyboard

    std::string m_url;           // current URL being typed
    std::string m_message;       // status or error message
    bool m_connected = false;
    bool m_connecting = false;
    ServerInfo m_serverInfo;
    std::string m_serverUrl;

    // Keyboard navigation
    std::vector<Key> m_keys;
    int m_activeKeyRow = 0;
    int m_activeKeyCol = 0;
    int m_maxCols = 0;

    // Connection attempt thread
    std::thread m_connectThread;
    std::atomic<bool> m_connectDone{false};
    std::atomic<bool> m_connectSuccess{false};
    std::string m_connectError;
    ServerInfo m_connectResult;
    bool m_finished = false;
    Uint32 m_infoTimer = 0;

    void buildKeyboard();
    int keyIndex(int row, int col) const;
    const Key *activeKey() const;
    void pressKey(const Key &key);
    void startConnection();
    void finishConnection();

    // Drawing helpers
    void drawInputField(SDL_Surface *fb);
    void drawKeyboard(SDL_Surface *fb);
    void drawStatus(SDL_Surface *fb);
};

} // namespace miyoofin

#endif // MIYOOFIN_SERVER_ENTRY_SCREEN_HPP