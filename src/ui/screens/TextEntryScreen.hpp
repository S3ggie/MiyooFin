#ifndef MIYOOFIN_TEXT_ENTRY_SCREEN_HPP
#define MIYOOFIN_TEXT_ENTRY_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../OnScreenKeyboard.hpp"
#include <functional>
#include <string>

namespace miyoofin {

/// Asks for a line of text on the on-screen keyboard (a playlist name, for example). A types,
/// X clears, START (or the [DONE] key) accepts, B cancels. `onDone` runs once, with the text,
/// when accepted; a cancel just pops the screen.
class TextEntryScreen : public Screen
{
  public:
    using Callback = std::function<void(const std::string& text)>;
    TextEntryScreen(std::string title, std::string initial, Callback onDone,
                    std::size_t maxLength = 60);

    bool handleAction(Action action) override;
    void update(Uint32) override {}
    void render(SDL_Surface* fb) override;
    const char* diagnosticName() const override
    {
        return "TextEntryScreen";
    }
    const std::string& text() const
    {
        return m_text;
    }

  private:
    OnScreenKeyboard m_keyboard;
    std::string m_title, m_text, m_hint;
    Callback m_onDone;
    std::size_t m_max;
};

} // namespace miyoofin

#endif
