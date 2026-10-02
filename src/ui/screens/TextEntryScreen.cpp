#include "TextEntryScreen.hpp"
#include "../../app/ScreenStack.hpp"
#include "../BitmapFont.hpp"
#include "../UiKit.hpp"

namespace miyoofin {

static const OnScreenKeyboard::Config kTextKeyboardConfig = {"[DONE]", 122};

TextEntryScreen::TextEntryScreen(std::string title, std::string initial, Callback onDone,
                                 std::size_t maxLength)
    : m_keyboard(kTextKeyboardConfig), m_title(std::move(title)), m_text(std::move(initial)),
      m_onDone(std::move(onDone)), m_max(maxLength)
{}

bool TextEntryScreen::handleAction(Action action)
{
    if (action == Action::Back || action == Action::Menu) {
        if (m_stack)
            m_stack->pop();
        return true;
    }
    const int result = m_keyboard.handleAction(action);
    if (result == -1)
        return false;
    if (result == 0)
        return true;
    const char c = static_cast<char>(result);
    if (c == OnScreenKeyboard::KEY_DEL) {
        if (!m_text.empty())
            m_text.pop_back();
        m_hint.clear();
    } else if (c == OnScreenKeyboard::KEY_CLR) {
        m_text.clear();
        m_hint.clear();
    } else if (c == OnScreenKeyboard::KEY_SUBMIT) {
        // Names with only spaces are not names.
        const std::size_t first = m_text.find_first_not_of(' ');
        if (first == std::string::npos) {
            m_hint = "Type a name first";
            return true;
        }
        const std::size_t last = m_text.find_last_not_of(' ');
        const std::string trimmed = m_text.substr(first, last - first + 1);
        if (m_onDone)
            m_onDone(trimmed);
        if (m_stack)
            m_stack->pop();
    } else if (c == OnScreenKeyboard::KEY_CANCEL) {
        m_hint.clear();
    } else if (m_text.size() < m_max) {
        m_text += c;
        m_hint.clear();
    }
    return true;
}

void TextEntryScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = m_title;
    header.showStatus = false;
    ui::header(fb, header);
    ui::inputField(fb, d::kMargin, 56, d::kScreenW - 2 * d::kMargin, 40, "Name", m_text, true,
                   false, 5 * BitmapFont::GLYPH_W);
    m_keyboard.render(fb);
    if (!m_hint.empty())
        ui::text(fb, d::kMargin, m_keyboard.keyboardBottom() + 14, m_hint, d::kDanger);
    ui::FooterSpec footer;
    footer.showLink = false;
    footer.hints = {{ui::Key::A, "Type"},
                    {ui::Key::B, "Cancel"},
                    {ui::Key::X, "Clear"},
                    {ui::Key::L2, "Caps"},
                    {ui::Key::Start, "Done"}};
    ui::footer(fb, footer);
}

} // namespace miyoofin
