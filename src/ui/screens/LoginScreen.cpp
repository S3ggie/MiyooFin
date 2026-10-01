#include "LoginScreen.hpp"
#include "../UiKit.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "../../net/JellyfinApi.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace miyoofin {

static const OnScreenKeyboard::Config kLoginKeyboardConfig = {"SIGN IN", 134};

LoginScreen::LoginScreen(const std::string& serverUrl, const std::string& serverName,
                         const std::string& deviceId, const std::string& initialMessage)
    : m_keyboard(kLoginKeyboardConfig), m_serverUrl(serverUrl), m_serverName(serverName),
      m_deviceId(deviceId), m_message(initialMessage)
{}

LoginScreen::~LoginScreen()
{
    m_loginWorker.join();
}

std::string& LoginScreen::activeText()
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

    // A second Sign In must never start a competing worker (SIGABRT,
    // "exited unexpectedly (134)" before WorkerSlot). Checked before mutating
    // state so refusing a still-running worker changes nothing; it never
    // blocks the UI thread on network I/O (unreachable via handleAction: it
    // gates on !m_connecting, and Back is swallowed while connecting).
    if (m_loginWorker.busy() && !m_loginWorker.reap())
        return;

    m_connecting = true;
    m_loginSuccess = false;
    m_loginError.clear();
    m_message = "Signing in...";

    std::string url = m_serverUrl;
    std::string user = m_username;
    std::string pass = m_password;
    std::string devId = m_deviceId;

    m_loginWorker.start([this, url, user, pass, devId](const CancelToken&) {
        AuthResult result;
        AuthError err;
        std::string errMsg;
        bool ok = JellyfinApi::authenticateByName(url, user, pass, devId, result, err, errMsg);
        if (ok) {
            m_loginResult = result;
            m_loginSuccess = true;
        } else {
            m_loginError = errMsg;
            m_loginSuccess = false;
        }
    });
}

void LoginScreen::finishLogin()
{
    // update() reaped the finished worker, which also establishes the
    // happens-before edge for m_loginError/m_loginResult.
    m_connecting = false;

    if (m_loginSuccess) {
        m_success = true;
        m_result = m_loginResult;
        printf("[LoginScreen] Sign-in successful for user '%s'\n", m_result.userName.c_str());
        return;
    }

    m_message = m_loginError;
    printf("[LoginScreen] Sign-in failed: %s\n", m_loginError.c_str());
}

void LoginScreen::enter()
{
    m_keyboard.reset();
    printf("[LoginScreen] enter server=%s\n", m_serverName.c_str());
}

void LoginScreen::leave()
{
    m_keyboard.reset();
    m_loginWorker.join();
}

bool LoginScreen::handleAction(Action action)
{
    if (m_connecting)
        return true;
    if (m_success) {
        m_finished = true;
        return false;
    }

    // --- Field-select mode ---
    if (m_inFields) {
        switch (action) {
        case Action::PrevPage:
            m_keyboard.handleAction(action); // toggle caps
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
            // The stack holds only this screen (App popped ServerEntry/
            // Connect first), so going back pushes a fresh ServerEntryScreen
            // via App (see wantsServerEntry()) instead of popping.
            m_wantsServerEntry = true;
            return true;
        case Action::Settings:
            if (!m_connecting && !m_success)
                submitLogin();
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
    if (result == -1)
        return false;
    if (result == 0)
        return true;

    // Process character
    char c = static_cast<char>(result);
    std::string& field = activeText();

    if (c == OnScreenKeyboard::KEY_DEL) {
        if (!field.empty())
            field.pop_back();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_CLR) {
        field.clear();
        m_message.clear();
        return true;
    }
    if (c == OnScreenKeyboard::KEY_SUBMIT) {
        if (!m_connecting && !m_success)
            submitLogin();
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
    if (m_connecting && m_loginWorker.reap()) {
        finishLogin();
    }
}

void LoginScreen::render(SDL_Surface* fb)
{
    ui::fill(fb, 0, 0, design::kScreenW, design::kScreenH, design::kCanvas);
    drawTitle(fb);
    drawInputFields(fb);
    m_keyboard.render(fb);
    drawStatus(fb);
    drawHints(fb);
}

// -------------------------------------------------------------------
// Drawing helpers
// -------------------------------------------------------------------

void LoginScreen::drawTitle(SDL_Surface* fb)
{
    ui::HeaderSpec header;
    header.title = "Sign in to " + m_serverName;
    header.showStatus = false;
    ui::header(fb, header);
}

void LoginScreen::drawField(SDL_Surface* fb, int y, const char* label, const std::string& display,
                            bool selected, bool masked)
{
    ui::inputField(fb, design::kMargin, y, design::kScreenW - 2 * design::kMargin, 32, label,
                   display, selected && m_inFields, masked, 10 * BitmapFont::GLYPH_W);
}

void LoginScreen::drawInputFields(SDL_Surface* fb)
{
    drawField(fb, 52, "Username", m_username, m_activeField == 0, false);
    drawField(fb, 90, "Password", m_password, m_activeField == 1, true);
}

void LoginScreen::drawStatus(SDL_Surface* fb)
{
    if (m_message.empty() && !m_connecting)
        return;
    const int y = m_keyboard.keyboardBottom() + 14;
    if (m_connecting) {
        const int dots = static_cast<int>((SDL_GetTicks() / 400) % 4);
        const std::string text = "Signing in" + std::string(static_cast<std::size_t>(dots), '.');
        ui::statusDot(fb, design::kMargin, y + 4, 8, design::kAccent);
        ui::text(fb, design::kMargin + 16, y, text, design::kAccentHi);
    } else {
        ui::statusDot(fb, design::kMargin, y + 4, 8, design::kDanger);
        int ly = y;
        for (const std::string& line :
             ui::wrap(m_message, design::kScreenW - 2 * design::kMargin - 16, 2)) {
            ui::text(fb, design::kMargin + 16, ly, line, design::kDanger);
            ly += 18;
        }
    }
}

void LoginScreen::drawHints(SDL_Surface* fb)
{
    ui::FooterSpec footer;
    footer.showLink = false;
    using ui::Key;
    if (m_inFields)
        footer.hints = {{Key::Dpad, "Switch field / keys"}, {Key::L2, "Caps"},
                        {Key::Start, "Sign in"}};
    else
        footer.hints = {{Key::A, "Type"}, {Key::B, "Delete"}, {Key::X, "Clear"},
                        {Key::L2, "Caps"}, {Key::Start, "Sign in"}};
    ui::footer(fb, footer);
}

} // namespace miyoofin
