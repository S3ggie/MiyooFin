#include "ServerEntryScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include "../../app/ScreenStack.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace miyoofin {

static const OnScreenKeyboard::Config kServerKeyboardConfig = {
    "[DONE]", 104
};

ServerEntryScreen::ServerEntryScreen()
    : m_keyboard(kServerKeyboardConfig)
    , m_url()
{
}

ServerEntryScreen::ServerEntryScreen(const std::string &initialUrl,
                                     const std::string &errorMsg)
    : m_keyboard(kServerKeyboardConfig)
    , m_url(initialUrl)
    , m_message(errorMsg)
{
}

ServerEntryScreen::ServerEntryScreen(const std::string &initialUrl,
                                     const std::string &errorMsg,
                                     const std::string &expectedServerId,
                                     bool localAddressEntry, bool publicAddressEntry)
    : m_keyboard(kServerKeyboardConfig)
    , m_url(initialUrl)
    , m_message(errorMsg)
    , m_expectedServerId(expectedServerId)
    , m_localAddressEntry(localAddressEntry)
    , m_publicAddressEntry(publicAddressEntry)
{
}

ServerEntryScreen::~ServerEntryScreen()
{
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
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
    m_connecting = true;
    m_connectDone = false;
    m_connectSuccess = false;
    m_connectError.clear();
    m_message = "Connecting...";

    std::string urlCopy = normalised;
    m_connectThread = std::thread([this, urlCopy]() {
        ServerInfo info;
        std::string err;
        bool ok = JellyfinApi::getSystemInfo(urlCopy, info, err);
        if (ok) { m_connectResult = info; m_connectSuccess = true; }
        else { m_connectError = err; m_connectSuccess = false; }
        m_connectDone = true;
    });
}

void ServerEntryScreen::finishConnection()
{
    m_connecting = false;
    if (m_connectSuccess) {
        if ((m_localAddressEntry || m_publicAddressEntry) && !JellyfinApi::serverIdsMatch(
                m_expectedServerId, m_connectResult.serverId)) {
            m_message = "Different Jellyfin server";
            return;
        }
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_serverUrl = m_url;
        m_infoTimer = 2000;
        if (!m_localAddressEntry && !m_publicAddressEntry) {
            FILE *f = fopen("server.txt", "w");
            if (f) { fprintf(f, "%s\n", m_serverUrl.c_str()); fclose(f); }
        }
        printf("[ServerEntry] Connected to %s (%s v%s)\n",
               m_serverUrl.c_str(), m_serverInfo.serverName.c_str(), m_serverInfo.version.c_str());
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Connection failed: %s",
                      m_connectError.c_str());
        m_message = buf;
    }
}

void ServerEntryScreen::cancelAddressEntry()
{
    // Settings address entry is an edit transaction: abandoning it must not
    // trigger verification or hand a value back to App for persistence.
    if (!m_localAddressEntry && !m_publicAddressEntry) return;
    m_addressEntryCancelled = true;
    if (m_stack) m_stack->pop();
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
    if (m_connectThread.joinable()) m_connectThread.join();
}

bool ServerEntryScreen::handleAction(Action action)
{
    if (m_connecting) return true;
    if (m_connected) { m_finished = true; return false; }

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
    if (result == -1) return false;
    if (result == 0) return true;  // navigation / caps toggle

    // Process character
    char c = static_cast<char>(result);
    if (c == OnScreenKeyboard::KEY_DEL) {
        if (!m_url.empty()) m_url.pop_back();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CLR) {
        m_url.clear();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_SUBMIT) {
        if (!m_connecting && !m_connected) startConnection();
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
    if (m_connecting && m_connectDone) {
        if (m_connectThread.joinable()) m_connectThread.join();
        finishConnection();
    }
    if (m_connected && !m_finished) {
        if (dt >= m_infoTimer) { m_infoTimer = 0; m_finished = true; }
        else { m_infoTimer -= dt; }
    }
}

void ServerEntryScreen::render(SDL_Surface *fb)
{
    BitmapFont::drawString(fb, 8, 8,
        m_localAddressEntry ? "Enter Local Jellyfin Address" : "Enter Jellyfin Server URL",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    const char *hints = (m_localAddressEntry || m_publicAddressEntry)
        ? "A=Type  B=Back  [DEL]=Delete  X=Clear  L2=Caps  START=Done"
        : "A=Type  B=Delete  X=Clear  L2=Caps  START=Done  SELECT=Cancel";
    BitmapFont::drawString(fb, 8, 28, hints,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    drawInputField(fb);
    m_keyboard.render(fb);
    drawStatus(fb);
}

void ServerEntryScreen::drawInputField(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 8, 50, 624, 38, 40, 40, 50, 255);
    BitmapFont::drawRect(fb, 8, 50, 624, 38,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B);
    int maxChars = (624 - 8) / BitmapFont::GLYPH_W;
    std::string display = m_url;
    if ((int)display.size() > maxChars)
        display = display.substr((int)display.size() - maxChars);
    BitmapFont::drawString(fb, 12, 61, display.c_str(),
        220, 220, 220, 40, 40, 50);
    int cursorX = 12 + (int)display.size() * BitmapFont::GLYPH_W;
    if (cursorX < 620)
        BitmapFont::fillRect(fb, cursorX, 61, 8, 16, 170, 170, 220, 255);
}

void ServerEntryScreen::drawStatus(SDL_Surface *fb)
{
    if (m_message.empty() && !m_connected) return;
    int y = m_keyboard.keyboardBottom() + 16;

    if (m_connected) {
        char line1[128];
        std::snprintf(line1, sizeof(line1), "Connected to %s",
                      m_serverInfo.serverName.c_str());
        BitmapFont::drawString(fb, 8, y, line1,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        y += BitmapFont::GLYPH_H + 2;
        char line2[128];
        std::snprintf(line2, sizeof(line2), "Jellyfin version %s",
                      m_serverInfo.version.c_str());
        BitmapFont::drawString(fb, 8, y, line2,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        y += BitmapFont::GLYPH_H + 2;
        char line3[128];
        std::snprintf(line3, sizeof(line3), "Server: %s", m_serverUrl.c_str());
        BitmapFont::drawString(fb, 8, y, line3,
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
        y += BitmapFont::GLYPH_H + 6;
        BitmapFont::drawString(fb, 8, y, "Press any button to continue...",
            Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
    } else if (m_connecting) {
        static int dotPhase = 0;
        dotPhase = (dotPhase + 1) % 40;
        int dots = dotPhase / 8;
        char buf[32] = "Connecting";
        for (int i = 0; i < dots; ++i) std::strcat(buf, ".");
        BitmapFont::drawString(fb, 8, y, buf,
            Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B);
    } else {
        BitmapFont::drawString(fb, 8, y, m_message.c_str(),
            Theme::HIGHLIGHT_R, Theme::HIGHLIGHT_G, Theme::HIGHLIGHT_B,
            Theme::BG_R, Theme::BG_G, Theme::BG_B, 75);
    }
}

} // namespace miyoofin
