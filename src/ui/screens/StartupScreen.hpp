#ifndef MIYOOFIN_STARTUP_SCREEN_HPP
#define MIYOOFIN_STARTUP_SCREEN_HPP

#include "../../app/Screen.hpp"

namespace miyoofin {

/// Shows the application title and version briefly.
/// In Checkpoint A this is a simple splash before the diagnostics screen.
class StartupScreen : public Screen {
public:
    void enter() override;
    void leave() override;
    bool handleAction(Action action) override;
    void update(Uint32 dt) override;
    void render(SDL_Surface *fb) override;

private:
    Uint32 m_age = 0;  // milliseconds since enter
};

} // namespace miyoofin

#endif // MIYOOFIN_STARTUP_SCREEN_HPP