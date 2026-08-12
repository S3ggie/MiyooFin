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

ServerEntryScreen::ServerEntryScreen()
    : m_url("https://")
{
    buildKeyboard();
}

ServerEntryScreen::ServerEntryScreen(const std::string &initialUrl,
                                     const std::string &errorMsg)
    : m_url(initialUrl.empty() ? "https://" : initialUrl)
    , m_message(errorMsg)
{
    buildKeyboard();
}

ServerEntryScreen::ServerEntryScreen(const std::string &initialUrl,
                                     const std::string &errorMsg,
                                     const std::string &expectedServerId,
                                     bool localAddressEntry)
    : m_url(localAddressEntry ? initialUrl : (initialUrl.empty() ? "https://" : initialUrl))
    , m_message(errorMsg)
    , m_expectedServerId(expectedServerId)
    , m_localAddressEntry(localAddressEntry)
{
    buildKeyboard();
}

ServerEntryScreen::~ServerEntryScreen()
{
    if (m_connectThread.joinable()) {
        m_connectThread.join();
    }
}

void ServerEntryScreen::buildKeyboard()
{
    m_keys.clear();
    struct KeyDef { const char *label; char ch; int row; };
    static const KeyDef defs[] = {
        {"1",'1',0},{"2",'2',0},{"3",'3',0},{"4",'4',0},{"5",'5',0},
        {"6",'6',0},{"7",'7',0},{"8",'8',0},{"9",'9',0},{"0",'0',0},
        {"q",'q',1},{"w",'w',1},{"e",'e',1},{"r",'r',1},{"t",'t',1},
        {"y",'y',1},{"u",'u',1},{"i",'i',1},{"o",'o',1},{"p",'p',1},
        {"a",'a',2},{"s",'s',2},{"d",'d',2},{"f",'f',2},{"g",'g',2},
        {"h",'h',2},{"j",'j',2},{"k",'k',2},{"l",'l',2},
        {"z",'z',3},{"x",'x',3},{"c",'c',3},{"v",'v',3},{"b",'b',3},
        {"n",'n',3},{"m",'m',3},
        {".",'.',4},{":",':',4},{"/",'/',4},{"-",'-',4},{"_",'_',4},
        {"~",'~',4},
        {"[DEL]",'\b',5},{"[CLR]",0x7F,5},{"[DONE]",0x01,5},
        {"[CANCEL]",0x02,5},
    };

    int colPtr[10] = {0};
    for (const auto &d : defs) {
        Key k;
        k.ch = d.ch; k.label = d.label; k.row = d.row;
        k.col = colPtr[d.row]++;
        m_keys.push_back(k);
        if (k.col > m_maxCols) m_maxCols = k.col;
    }
    m_activeKeyRow = 0;
    m_activeKeyCol = 0;
}

std::string ServerEntryScreen::keyLabel(const Key &key) const
{
    std::string label(key.label);
    if (m_caps && label.size() == 1 && std::isalpha(
            static_cast<unsigned char>(label[0]))) {
        label[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(label[0])));
    }
    return label;
}

int ServerEntryScreen::keyIndex(int row, int col) const
{
    for (int i = 0; i < (int)m_keys.size(); ++i) {
        if (m_keys[i].row == row && m_keys[i].col == col)
            return i;
    }
    return -1;
}

const ServerEntryScreen::Key *ServerEntryScreen::activeKey() const
{
    int idx = keyIndex(m_activeKeyRow, m_activeKeyCol);
    if (idx < 0) return nullptr;
    return &m_keys[idx];
}

void ServerEntryScreen::pressKey(const Key &key)
{
    char c = key.ch;
    if (m_caps && std::isalpha(static_cast<unsigned char>(c)))
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (c == '\b') {
        if (!m_url.empty()) {
            m_url.pop_back();
            if (m_url.empty() || m_url == "https" || m_url == "https:" ||
                m_url == "http" || m_url == "http:")
                m_url = "https://";
        }
        m_message.clear();
        return;
    }
    if (c == 0x7F) { m_url = m_localAddressEntry ? "" : "https://"; m_message.clear(); return; }
    if (c == 0x01) {
        if (!m_connecting && !m_connected) startConnection();
        return;
    }
    if (c == 0x02) { m_message.clear(); return; }
    m_url += c;
    m_message.clear();
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
        if (m_localAddressEntry && !JellyfinApi::serverIdsMatch(
                m_expectedServerId, m_connectResult.serverId)) {
            m_message = "Different Jellyfin server";
            return;
        }
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_serverUrl = m_url;
        m_infoTimer = 2000;
        if (!m_localAddressEntry) {
            FILE *f = fopen("server.txt", "w");
            if (f) { fprintf(f, "%s\n", m_serverUrl.c_str()); fclose(f); }
        }
        printf("[ServerEntry] Connected to %s (%s v%s)\n",
               m_serverUrl.c_str(), m_serverInfo.serverName.c_str(),
               m_serverInfo.version.c_str());
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Connection failed: %s",
                      m_connectError.c_str());
        m_message = buf;
    }
}

void ServerEntryScreen::enter()
{
    m_caps = false;
    printf("[ServerEntryScreen] enter\n");
}

void ServerEntryScreen::leave()
{
    m_caps = false;
    printf("[ServerEntryScreen] leave\n");
    if (m_connectThread.joinable()) m_connectThread.join();
}

bool ServerEntryScreen::handleAction(Action action)
{
    if (m_connecting) return true;
    if (m_connected) { m_finished = true; return false; }

    switch (action) {
    case Action::PrevPage:
        m_caps = !m_caps;
        return true;
    case Action::Up:
        if (m_activeKeyRow > 0) {
            m_activeKeyRow--;
            if (keyIndex(m_activeKeyRow, m_activeKeyCol) < 0) {
                int c = m_activeKeyCol;
                while (c >= 0 && keyIndex(m_activeKeyRow, c) < 0) c--;
                m_activeKeyCol = (c >= 0) ? c : 0;
            }
        }
        return true;
    case Action::Down: {
        int nr = m_activeKeyRow + 1;
        while (nr < 6 && keyIndex(nr, 0) < 0) nr++;
        if (keyIndex(nr, 0) >= 0) {
            m_activeKeyRow = nr;
            if (keyIndex(m_activeKeyRow, m_activeKeyCol) < 0) {
                int c = 0;
                while (keyIndex(m_activeKeyRow, c) < 0) c++;
                m_activeKeyCol = c;
            }
        }
        return true;
    }
    case Action::Left:
        if (m_activeKeyCol > 0) {
            m_activeKeyCol--;
            while (m_activeKeyCol > 0 && keyIndex(m_activeKeyRow, m_activeKeyCol) < 0)
                m_activeKeyCol--;
        }
        return true;
    case Action::Right: {
        int nc = m_activeKeyCol + 1;
        if (keyIndex(m_activeKeyRow, nc) >= 0) m_activeKeyCol = nc;
        return true;
    }
    case Action::Confirm: {
        const Key *k = activeKey();
        if (k) pressKey(*k);
        return true;
    }
    case Action::Back:
        pressKey(Key{'\b',"[DEL]",0,0}); return true;
    case Action::Search:
        pressKey(Key{0x7F,"[CLR]",0,0}); return true;
    case Action::Settings:
        pressKey(Key{0x01,"[DONE]",0,0}); return true;
    case Action::Menu:
        pressKey(Key{0x02,"[CANCEL]",0,0}); return true;
    default:
        return false;
    }
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
    BitmapFont::drawString(fb, 8, 8, m_localAddressEntry ? "Enter Local Jellyfin Address" : "Enter Jellyfin Server URL",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::drawString(fb, 8, 28,
        "A=Type  B=Delete  X=Clear  L2=Caps  START=Done  SELECT=Cancel",
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    drawInputField(fb);
    drawKeyboard(fb);
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

void ServerEntryScreen::drawKeyboard(SDL_Surface *fb)
{
    int maxRow = 0;
    for (const auto &k : m_keys) if (k.row > maxRow) maxRow = k.row;

    for (int row = 0; row <= maxRow; ++row) {
        int firstCol = 1000, lastCol = -1;
        for (const auto &k : m_keys) {
            if (k.row == row) {
                if (k.col < firstCol) firstCol = k.col;
                if (k.col > lastCol) lastCol = k.col;
            }
        }
        if (firstCol > lastCol) continue;
        int rkc = lastCol - firstCol + 1;
        int rowW = 0;
        for (const auto &k : m_keys) {
            if (k.row != row) continue;
            const std::string label = keyLabel(k);
            const int keyW = (k.ch == '\b' || k.ch == 0x7F ||
                              k.ch == 0x01 || k.ch == 0x02)
                ? (int)label.size() * BitmapFont::GLYPH_W * KEY_LABEL_SCALE + 16
                : KEY_W;
            rowW += keyW;
        }
        rowW += (rkc - 1) * KEY_GAP;
        int startX = (640 - rowW) / 2;
        int x = startX;

        for (const auto &k : m_keys) {
            if (k.row != row) continue;
            int y = KEYBOARD_TOP + row * (KEY_H + KEY_GAP);
            bool sel = (k.row == m_activeKeyRow && k.col == m_activeKeyCol);
            Uint8 bgR = sel ? Theme::ACCENT_R : 40;
            Uint8 bgG = sel ? Theme::ACCENT_G : 40;
            Uint8 bgB = sel ? Theme::ACCENT_B : 50;
            Uint8 fgR = sel ? 255 : Theme::TEXT_R;
            Uint8 fgG = sel ? 255 : Theme::TEXT_G;
            Uint8 fgB = sel ? 255 : Theme::TEXT_B;
            const std::string label = keyLabel(k);
            int keyW = KEY_W;
            if (k.ch == '\b' || k.ch == 0x7F || k.ch == 0x01 || k.ch == 0x02)
                keyW = (int)label.size() * BitmapFont::GLYPH_W * KEY_LABEL_SCALE + 16;
            BitmapFont::fillRect(fb, x, y, keyW, KEY_H, bgR, bgG, bgB, 255);
            BitmapFont::drawRect(fb, x, y, keyW, KEY_H, fgR, fgG, fgB);
            int labelW = (int)label.size() * BitmapFont::GLYPH_W * KEY_LABEL_SCALE;
            int lx = x + (keyW - labelW) / 2;
            int ly = y + (KEY_H - BitmapFont::GLYPH_H * KEY_LABEL_SCALE) / 2;
            BitmapFont::drawStringScaled(fb, lx, ly, label.c_str(), KEY_LABEL_SCALE,
                                         fgR, fgG, fgB, bgR, bgG, bgB);
            x += keyW + KEY_GAP;
        }
    }
    BitmapFont::drawString(fb, 8, 88, m_caps ? "CAPS ON" : "CAPS OFF",
        m_caps ? Theme::ACCENT_R : Theme::TEXT_R,
        m_caps ? Theme::ACCENT_G : Theme::TEXT_G,
        m_caps ? Theme::ACCENT_B : Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void ServerEntryScreen::drawStatus(SDL_Surface *fb)
{
    if (m_message.empty() && !m_connected) return;
    int y = KEYBOARD_TOP + 6 * (KEY_H + KEY_GAP) + 16;

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
