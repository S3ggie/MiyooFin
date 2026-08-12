#include "LoginScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace miyoofin {

LoginScreen::LoginScreen(const std::string &serverUrl,
                         const std::string &serverName,
                         const std::string &deviceId,
                         const std::string &initialMessage)
    : m_serverUrl(serverUrl)
    , m_serverName(serverName)
    , m_deviceId(deviceId)
    , m_message(initialMessage)
{
    buildKeyboard();
}

LoginScreen::~LoginScreen()
{
    if (m_loginThread.joinable()) {
        m_loginThread.join();
    }
}

void LoginScreen::buildKeyboard()
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
        {".",'.',4},{":",':',4},{"-",'-',4},{"_",'_',4},{"@",'@',4},
        {"!",'!',4},
        {"[DEL]",'\b',5},{"[CLR]",0x7F,5},{"[SIGN IN]",0x01,5},
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

std::string LoginScreen::keyLabel(const Key &key) const
{
    std::string label(key.label);
    if (m_caps && label.size() == 1 && std::isalpha(
            static_cast<unsigned char>(label[0]))) {
        label[0] = static_cast<char>(std::toupper(
            static_cast<unsigned char>(label[0])));
    }
    return label;
}

int LoginScreen::keyIndex(int row, int col) const
{
    for (int i = 0; i < (int)m_keys.size(); ++i) {
        if (m_keys[i].row == row && m_keys[i].col == col)
            return i;
    }
    return -1;
}

const LoginScreen::Key *LoginScreen::activeKey() const
{
    int idx = keyIndex(m_activeKeyRow, m_activeKeyCol);
    if (idx < 0) return nullptr;
    return &m_keys[idx];
}

std::string &LoginScreen::activeText()
{
    return (m_activeField == 0) ? m_username : m_password;
}

void LoginScreen::pressKey(const Key &key)
{
    char c = key.ch;
    if (m_caps && std::isalpha(static_cast<unsigned char>(c)))
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string &field = activeText();

    if (c == '\b') {
        if (!field.empty()) {
            field.pop_back();
        }
        m_message.clear();
        return;
    }
    if (c == 0x7F) {
        field.clear();
        m_message.clear();
        return;
    }
    if (c == 0x01) {
        // SIGN IN
        if (!m_connecting && !m_success) submitLogin();
        return;
    }
    if (c == 0x02) {
        // CANCEL — clear message
        m_message.clear();
        return;
    }
    field += c;
    m_message.clear();
}

void LoginScreen::submitLogin()
{
    if (m_username.empty()) {
        m_message = "Please enter your username";
        return;
    }
    if (m_password.empty()) {
        m_message = "Please enter your password";
        return;
    }

    m_connecting = true;
    m_loginDone = false;
    m_loginSuccess = false;
    m_loginError.clear();
    m_message = "Signing in...";

    std::string url = m_serverUrl;
    std::string user = m_username;
    std::string pass = m_password;
    std::string devId = m_deviceId;

    m_loginThread = std::thread([this, url, user, pass, devId]() {
        AuthResult result;
        AuthError err;
        std::string errMsg;
        bool ok = JellyfinApi::authenticateByName(url, user, pass, devId,
                                                  result, err, errMsg);
        if (ok) {
            m_loginResult = result;
            m_loginSuccess = true;
        } else {
            m_loginError = errMsg;
            m_loginSuccess = false;
        }
        m_loginDone = true;
    });
}

void LoginScreen::finishLogin()
{
    m_loginDone = false;
    m_connecting = false;

    if (m_loginSuccess) {
        m_success = true;
        m_result = m_loginResult;
        printf("[LoginScreen] Sign-in successful for user '%s'\n",
               m_result.userName.c_str());
        return;
    }

    m_message = m_loginError;
}

void LoginScreen::enter()
{
    m_caps = false;
    printf("[LoginScreen] enter server=%s\n", m_serverName.c_str());
}

void LoginScreen::leave()
{
    m_caps = false;
    if (m_loginThread.joinable()) {
        m_loginThread.join();
    }
}

bool LoginScreen::handleAction(Action action)
{
    if (m_connecting) return true;
    if (m_success) { m_finished = true; return false; }

    // --- Field-select mode ---
    if (m_inFields) {
        switch (action) {
        case Action::PrevPage:
            m_caps = !m_caps;
            return true;
        case Action::Up:
        case Action::Down:
        case Action::Confirm:
            m_inFields = false;
            m_activeKeyRow = 0;
            if (keyIndex(0, m_activeKeyCol) < 0) m_activeKeyCol = 0;
            return true;
        case Action::Left:
            m_activeField = 0;
            return true;
        case Action::Right:
            m_activeField = 1;
            return true;
        case Action::Back:
            m_message.clear();
            return true;
        case Action::Settings:
            if (!m_connecting && !m_success) submitLogin();
            return true;
        default:
            return false;
        }
    }

    // --- Keyboard mode ---
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
        } else {
            m_inFields = true;
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
        pressKey(Key{'\b', "[DEL]", 0, 0});
        return true;
    case Action::Search:
        pressKey(Key{0x7F, "[CLR]", 0, 0});
        return true;
    case Action::Settings:
        if (!m_connecting && !m_success) submitLogin();
        return true;
    case Action::Menu:
        pressKey(Key{0x02, "[CANCEL]", 0, 0});
        return true;
    default:
        return false;
    }
}

void LoginScreen::update(Uint32 dt)
{
    (void)dt;
    if (m_connecting && m_loginDone) {
        finishLogin();
    }
}
void LoginScreen::render(SDL_Surface *fb)
{
    drawTitle(fb);
    drawInputFields(fb);
    drawKeyboard(fb);
    drawStatus(fb);
    drawHints(fb);
}

// -------------------------------------------------------------------
// Drawing helpers
// -------------------------------------------------------------------

void LoginScreen::drawTitle(SDL_Surface *fb)
{
    char title[128];
    std::snprintf(title, sizeof(title), "Sign in to %s", m_serverName.c_str());
    BitmapFont::drawString(fb, 8, 8, title,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void LoginScreen::drawField(SDL_Surface *fb, int y, const char *label,
                             const std::string &display, bool selected,
                             bool masked)
{
    // Field background
    BitmapFont::fillRect(fb, 8, y, 624, 28, 40, 40, 50, 255);
    BitmapFont::drawRect(fb, 8, y, 624, 28,
        selected ? Theme::ACCENT_R : Theme::TEXT_R,
        selected ? Theme::ACCENT_G : Theme::TEXT_G,
        selected ? Theme::ACCENT_B : Theme::TEXT_B);

    // Label
    BitmapFont::drawString(fb, 12, y + 2, label,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        40, 40, 50);

    // Value
    int valX = 80;
    int maxChars = (624 - valX - 8) / BitmapFont::GLYPH_W;
    std::string displayStr;
    if (masked) {
        displayStr = std::string(display.size(), '*');
    } else {
        displayStr = display;
    }
    if ((int)displayStr.size() > maxChars) {
        displayStr = displayStr.substr((int)displayStr.size() - maxChars);
    }
    BitmapFont::drawString(fb, valX, y + 6, displayStr.c_str(),
        220, 220, 220, 40, 40, 50);

    // Cursor (only on the selected field when in field-select mode)
    if (selected && m_inFields) {
        int cursorX = valX + (int)displayStr.size() * BitmapFont::GLYPH_W;
        if (cursorX < 620)
            BitmapFont::fillRect(fb, cursorX, y + 6, 8, 16, 170, 170, 220, 255);
    }
}

void LoginScreen::drawInputFields(SDL_Surface *fb)
{
    drawField(fb, 36, "Username:", m_username, m_activeField == 0, false);
    drawField(fb, 70, "Password:", m_password, m_activeField == 1, true);
}

void LoginScreen::drawKeyboard(SDL_Surface *fb)
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
            rowW += (k.ch == '\b' || k.ch == 0x7F || k.ch == 0x01 || k.ch == 0x02)
                ? (int)label.size() * BitmapFont::GLYPH_W * KEY_LABEL_SCALE + 16
                : KEY_W;
        }
        rowW += (rkc - 1) * KEY_GAP;
        int x = (640 - rowW) / 2;

        for (const auto &k : m_keys) {
            if (k.row != row) continue;
            int y = KEYBOARD_TOP + row * (KEY_H + KEY_GAP);
            bool sel = !m_inFields && k.row == m_activeKeyRow && k.col == m_activeKeyCol;
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
    BitmapFont::drawString(fb, 8, 106, m_caps ? "CAPS ON" : "CAPS OFF",
        m_caps ? Theme::ACCENT_R : Theme::TEXT_R,
        m_caps ? Theme::ACCENT_G : Theme::TEXT_G,
        m_caps ? Theme::ACCENT_B : Theme::TEXT_B,
        Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

void LoginScreen::drawStatus(SDL_Surface *fb)
{
    if (m_message.empty() && !m_connecting) return;
    int y = KEYBOARD_TOP + 6 * (KEY_H + KEY_GAP) + 16;

    if (m_connecting) {
        static int dotPhase = 0;
        dotPhase = (dotPhase + 1) % 40;
        int dots = dotPhase / 8;
        char buf[32] = "Signing in";
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

void LoginScreen::drawHints(SDL_Surface *fb)
{
    int y = 480 - 24;
    BitmapFont::fillRect(fb, 0, y, 640, 24,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3, 255);

    const char *hints;
    if (m_inFields) {
        hints = "LEFT/RIGHT=Switch Field  L2=Caps  DOWN=Keyboard  START=Sign In";
    } else {
        hints = "A=Type  B=Delete  X=Clear  L2=Caps  START=Sign In";
    }
    BitmapFont::drawString(fb, 8, y + 4, hints,
        Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
        Theme::BG_R * 2 / 3, Theme::BG_G * 2 / 3, Theme::BG_B * 2 / 3);
}

} // namespace miyoofin
