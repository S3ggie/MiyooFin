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
        {"Q",'Q',1},{"W",'W',1},{"E",'E',1},{"R",'R',1},{"T",'T',1},
        {"Y",'Y',1},{"U",'U',1},{"I",'I',1},{"O",'O',1},{"P",'P',1},
        {"A",'A',2},{"S",'S',2},{"D",'D',2},{"F",'F',2},{"G",'G',2},
        {"H",'H',2},{"J",'J',2},{"K",'K',2},{"L",'L',2},
        {"Z",'Z',3},{"X",'X',3},{"C",'C',3},{"V",'V',3},{"B",'B',3},
        {"N",'N',3},{"M",'M',3},
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
    if (c == 0x7F) { m_url = "https://"; m_message.clear(); return; }
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
    m_connectThread.detach();
}

void ServerEntryScreen::finishConnection()
{
    m_connecting = false;
    if (m_connectSuccess) {
        m_connected = true;
        m_serverInfo = m_connectResult;
        m_serverUrl = m_url;
        m_infoTimer = 2000;
        FILE *f = fopen("server.txt", "w");
        if (f) { fprintf(f, "%s\n", m_serverUrl.c_str()); fclose(f); }
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
    printf("[ServerEntryScreen] enter\n");
}

void ServerEntryScreen::leave()
{
    printf("[ServerEntryScreen] leave\n");
    if (m_connectThread.joinable()) m_connectThread.join();
}

bool ServerEntryScreen::handleAction(Action action)
{
    if (m_connecting) return true;
    if (m_connected) { m_finished = true; return false; }

    switch (action) {
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
    if (m_connecting && m_connectDone) finishConnection();
    if (m_connected && !m_finished) {
        if (dt >= m_infoTimer) { m_infoTimer = 0; m_finished = true; }
        else { m_infoTimer -= dt; }
    }
}

void ServerEntryScreen::render(SDL_Surface *fb)
{
    BitmapFont::drawString(fb, 8, 8, "Enter Jellyfin Server URL",
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::drawString(fb, 8, 28,
        "A=Type  B=Delete  X=Clear  START=Done  SELECT=Cancel",
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
    drawInputField(fb);
    drawKeyboard(fb);
    drawStatus(fb);
}

void ServerEntryScreen::drawInputField(SDL_Surface *fb)
{
    BitmapFont::fillRect(fb, 8, 50, 624, 28, 40, 40, 50, 255);
    BitmapFont::drawRect(fb, 8, 50, 624, 28,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B);
    int maxChars = (624 - 8) / BitmapFont::GLYPH_W;
    std::string display = m_url;
    if ((int)display.size() > maxChars)
        display = display.substr((int)display.size() - maxChars);
    BitmapFont::drawString(fb, 12, 54, display.c_str(),
        220, 220, 220, 40, 40, 50);
    int cursorX = 12 + (int)display.size() * BitmapFont::GLYPH_W;
    if (cursorX < 620)
        BitmapFont::fillRect(fb, cursorX, 54, 8, 16, 170, 170, 220, 255);
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
        int startX = (640 - (rkc * KEY_W + (rkc - 1) * KEY_GAP)) / 2;

        for (const auto &k : m_keys) {
            if (k.row != row) continue;
            int x = startX + k.col * (KEY_W + KEY_GAP);
            int y = KEYBOARD_TOP + row * (KEY_H + KEY_GAP);
            bool sel = (k.row == m_activeKeyRow && k.col == m_activeKeyCol);
            Uint8 bgR = sel ? Theme::ACCENT_R : 40;
            Uint8 bgG = sel ? Theme::ACCENT_G : 40;
            Uint8 bgB = sel ? Theme::ACCENT_B : 50;
            Uint8 fgR = sel ? 255 : Theme::TEXT_R;
            Uint8 fgG = sel ? 255 : Theme::TEXT_G;
            Uint8 fgB = sel ? 255 : Theme::TEXT_B;
            int keyW = KEY_W;
            if (k.ch == '\b' || k.ch == 0x7F || k.ch == 0x01 || k.ch == 0x02)
                keyW = (int)::strlen(k.label) * BitmapFont::GLYPH_W + 8;
            BitmapFont::fillRect(fb, x, y, keyW, KEY_H, bgR, bgG, bgB, 255);
            BitmapFont::drawRect(fb, x, y, keyW, KEY_H, fgR, fgG, fgB);
            int lx = x + (keyW - (int)::strlen(k.label) * BitmapFont::GLYPH_W) / 2;
            int ly = y + (KEY_H - BitmapFont::GLYPH_H) / 2;
            BitmapFont::drawString(fb, lx, ly, k.label, fgR, fgG, fgB, bgR, bgG, bgB);
        }
    }
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
