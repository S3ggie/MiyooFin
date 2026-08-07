#ifndef MIYOOFIN_INPUT_DIAGNOSTICS_SCREEN_HPP
#define MIYOOFIN_INPUT_DIAGNOSTICS_SCREEN_HPP

#include "../../app/Screen.hpp"
#include "../../input/InputManager.hpp"
#include <vector>

namespace miyoofin {

/// Displays raw SDL input events so the real Miyoo button mapping
/// can be determined on-device.
class InputDiagnosticsScreen : public Screen {
public:
    explicit InputDiagnosticsScreen(InputManager *input);
    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

private:
    InputManager *m_input;
    std::vector<RawEvent> m_displayedEvents;
    int m_scrollOffset = 0;
};

} // namespace miyoofin

#endif // MIYOOFIN_INPUT_DIAGNOSTICS_SCREEN_HPP