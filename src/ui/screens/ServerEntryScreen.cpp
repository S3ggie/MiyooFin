#include "ServerEntryScreen.hpp"
#include "../UiKit.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../app/ScreenStack.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace miyoofin {

static const OnScreenKeyboard::Config kServerKeyboardConfig = {"[DONE]", 122};

ServerEntryScreen::ServerEntryScreen() : m_keyboard(kServerKeyboardConfig), m_url() {}

ServerEntryScreen::ServerEntryScreen(const std::string& initialUrl, const std::string& errorMsg)
    : m_keyboard(kServerKeyboardConfig), m_url(initialUrl), m_message(errorMsg)
{}

ServerEntryScreen::ServerEntryScreen(const std::string& initialUrl, const std::string& errorMsg,
                                     const std::string& expectedServerId, bool localAddressEntry,
                                     bool publicAddressEntry)
    : m_keyboard(kServerKeyboardConfig), m_url(initialUrl), m_message(errorMsg),
      m_expectedServerId(expectedServerId), m_localAddressEntry(localAddressEntry),
      m_publicAddressEntry(publicAddressEntry)
{}

ServerEntryScreen::~ServerEntryScreen()
{
    m_connectWorker.join();
}

void ServerEntryScreen::startConnection()
{
    if (m_localAddressEntry && m_url.empty()) {
        m_serverUrl.clear();
        m_connected = true;
        m_finished = true;
        return;
    }
    if (m_url.empty() || m_url == "http://" || m_url == "https://") {
        m_message = "Please enter a server URL";
        return;
    }
    std::string normalised = JellyfinApi::normaliseUrl(m_url);
    m_url = normalised;
    // A still-running worker refuses a second attempt instead of blocking
    // the UI; a finished one is reclaimed.
    if (m_connectWorker.busy() && !m_connectWorker.reap())
        return;
    m_connecting = true;
    m_connectSuccess = false;
    m_connectError.clear();
    m_message = "Connecting...";

    std::string urlCopy = normalised;
    m_connectWorker.start([this, urlCopy](const CancelToken&) {
        ServerInfo info;
        std::string err;
        bool ok = JellyfinApi::getSystemInfo(urlCopy, info, err);
        if (ok) {
            m_connectResult = info;
            m_connectSuccess = true;
        } else {
            m_connectError = err;
            m_connectSuccess = false;
        }
    });
}

void ServerEntryScreen::finishConnection()
{
    m_connecting = false;
    if (m_connectSuccess) {
        if ((m_localAddressEntry || m_publicAddressEntry) &&
            !JellyfinApi::serverIdsMatch(m_expectedServerId, m_connectResult.serverId)) {
            m_message = "Different Jellyfin server";
            return;
        }
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_serverUrl = m_url;
        m_infoTimer = 2000;
        if (!m_localAddressEntry && !m_publicAddressEntry) {
            FILE* f = fopen("server.txt", "w");
            if (f) {
                fprintf(f, "%s\n", m_serverUrl.c_str());
                fclose(f);
            }
        }
        printf("[ServerEntry] Connected to %s (%s v%s)\n", m_serverUrl.c_str(),
               m_serverInfo.serverName.c_str(), m_serverInfo.version.c_str());
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Connection failed: %s", m_connectError.c_str());
        m_message = buf;
    }
}

void ServerEntryScreen::cancelAddressEntry()
{
    // Settings address entry is an edit transaction: abandoning it must not
    // trigger verification or hand a value back to App for persistence.
    if (!m_localAddressEntry && !m_publicAddressEntry)
        return;
    m_addressEntryCancelled = true;
    if (m_stack)
        m_stack->pop();
}

void ServerEntryScreen::enter()
{
    m_keyboard.reset();
    printf("[ServerEntryScreen] enter\n");
}

void ServerEntryScreen::leave()
{
    m_keyboard.reset();
    printf("[ServerEntryScreen] leave\n");
    m_connectWorker.join();
}

bool ServerEntryScreen::handleAction(Action action)
{
    if (m_connecting)
        return true;
    if (m_connected) {
        m_finished = true;
        return false;
    }

    // Screen-specific overrides for address entry mode
    if (action == Action::Back && (m_localAddressEntry || m_publicAddressEntry)) {
        cancelAddressEntry();
        return true;
    }
    if (action == Action::Menu && (m_localAddressEntry || m_publicAddressEntry)) {
        cancelAddressEntry();
        return true;
    }

    // Delegate to shared keyboard
    int result = m_keyboard.handleAction(action);
    if (result == -1)
        return false;
    if (result == 0)
        return true; // navigation / caps toggle

    // Process character
    char c = static_cast<char>(result);
    if (c == OnScreenKeyboard::KEY_DEL) {
        if (!m_url.empty())
            m_url.pop_back();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CLR) {
        m_url.clear();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_SUBMIT) {
        if (!m_connecting && !m_connected)
            startConnection();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CANCEL) {
        m_message.clear();
        return true;
    }
    // Printable character (including space)
    m_url += c;
    m_message.clear();
    return true;
}

void ServerEntryScreen::update(Uint32 dt)
{
    if (m_connecting && m_connectWorker.reap()) {
        finishConnection();
    }
    if (m_connected && !m_finished) {
        if (dt >= m_infoTimer) {
            m_infoTimer = 0;
            m_finished = true;
        } else {
            m_infoTimer -= dt;
        }
    }
}

void ServerEntryScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = m_localAddressEntry    ? "Local Jellyfin address"
                   : m_publicAddressEntry ? "Public Jellyfin address"
                                          : "Connect to Jellyfin";
    header.showStatus = false;
    ui::header(fb, header);
    drawInputField(fb);
    m_keyboard.render(fb);
    drawStatus(fb);

    ui::FooterSpec footer;
    footer.showLink = false;
    using ui::Key;
    if (m_localAddressEntry || m_publicAddressEntry)
        footer.hints = {{Key::A, "Type"}, {Key::B, "Back"}, {Key::X, "Clear"}, {Key::L2, "Caps"},
                        {Key::Start, "Done"}};
    else
        footer.hints = {{Key::A, "Type"}, {Key::B, "Delete"}, {Key::X, "Clear"}, {Key::L2, "Caps"},
                        {Key::Start, "Done"}, {Key::Select, "Cancel"}};
    ui::footer(fb, footer);
}

void ServerEntryScreen::drawInputField(SDL_Surface* fb)
{
    ui::inputField(fb, design::kMargin, 56, design::kScreenW - 2 * design::kMargin, 40, "URL",
                   m_url, true, false, 5 * BitmapFont::GLYPH_W);
}

void ServerEntryScreen::drawStatus(SDL_Surface* fb)
{
    namespace d = design;
    if (m_message.empty() && !m_connected && !m_connecting)
        return;
    const int y = m_keyboard.keyboardBottom() + 14;
    if (m_connected) {
        // Success card in place of the keyboard's attention: name, version, address.
        ui::panel(fb, d::kMargin, y - 4, d::kScreenW - 2 * d::kMargin, 68,
                  ui::mix(d::kPanel, d::kSuccess, 10), d::kSuccess);
        ui::statusDot(fb, d::kMargin + 12, y + 6, 8, d::kSuccess);
        ui::text(fb, d::kMargin + 28, y, "Connected to " + m_serverInfo.serverName, d::kText);
        ui::text(fb, d::kMargin + 28, y + 20,
                 ui::fit("Jellyfin " + m_serverInfo.version + "  -  " + m_serverUrl,
                         d::kScreenW - 2 * d::kMargin - 40),
                 d::kTextSecondary);
        ui::text(fb, d::kMargin + 28, y + 40, "Press any button to continue", d::kTextMuted);
    } else if (m_connecting) {
        const int dots = static_cast<int>((SDL_GetTicks() / 400) % 4);
        ui::statusDot(fb, d::kMargin, y + 4, 8, d::kAccent);
        ui::text(fb, d::kMargin + 16, y, "Connecting" + std::string(static_cast<std::size_t>(dots), '.'),
                 d::kAccentHi);
    } else {
        ui::statusDot(fb, d::kMargin, y + 4, 8, d::kDanger);
        int ly = y;
        for (const std::string& line : ui::wrap(m_message, d::kScreenW - 2 * d::kMargin - 16, 2)) {
            ui::text(fb, d::kMargin + 16, ly, line, d::kDanger);
            ly += 18;
        }
    }
}

} // namespace miyoofin
