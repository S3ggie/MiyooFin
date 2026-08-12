#include "LoginScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace miyoofin {

static const OnScreenKeyboard::Config kLoginKeyboardConfig = {
    "SIGN IN", 134
};

LoginScreen::LoginScreen(const std::string &serverUrl,
                         const std::string &serverName,
                         const std::string &deviceId,
                         const std::string &initialMessage)
    : m_keyboard(kLoginKeyboardConfig)
    , m_serverUrl(serverUrl)
    , m_serverName(serverName)
    , m_deviceId(deviceId)
    , m_message(initialMessage)
{
}

LoginScreen::~LoginScreen()
{
    if (m_loginThread.joinable()) {
        m_loginThread.join();
    }
}

std::string &LoginScreen::activeText()
{
    return (m_activeField == 0) ? m_username : m_password;
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
    m_keyboard.reset();
    printf("[LoginScreen] enter server=%s\n", m_serverName.c_str());
}

void LoginScreen::leave()
{
    m_keyboard.reset();
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
            m_keyboard.handleAction(action);  // toggle caps
            return true;
        case Action::Up:
        case Action::Down:
        case Action::Confirm:
            m_inFields = false;
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
    // Up from row 0 returns to field-select mode
    if (action == Action::Up && m_keyboard.selectionRow() == 0) {
        m_inFields = true;
        return true;
    }

    int result = m_keyboard.handleAction(action);
    if (result == -1) return false;
    if (result == 0) return true;

    // Process character
    char c = static_cast<char>(result);
    std::string &field = activeText();

    if (c == OnScreenKeyboard::KEY_DEL) {
        if (!field.empty()) field.pop_back();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CLR) {
        field.clear();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_SUBMIT) {
        if (!m_connecting && !m_success) submitLogin();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CANCEL) {
        m_message.clear();
        return true;
    }
    // Printable character (including space)
    field += c;
    m_message.clear();
    return true;
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
    m_keyboard.render(fb);
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
    BitmapFont::fillRect(fb, 8, y, 624, 28, 40, 40, 50, 255);
    BitmapFont::drawRect(fb, 8, y, 624, 28,
        selected ? Theme::ACCENT_R : Theme::TEXT_R,
        selected ? Theme::ACCENT_G : Theme::TEXT_G,
        selected ? Theme::ACCENT_B : Theme::TEXT_B);

    BitmapFont::drawString(fb, 12, y + 2, label,
        Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
        40, 40, 50);

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

void LoginScreen::drawStatus(SDL_Surface *fb)
{
    if (m_message.empty() && !m_connecting) return;
    int y = m_keyboard.keyboardBottom() + 16;

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
