#include "../UiKit.hpp"
#include "StartupScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>

namespace miyoofin {

void StartupScreen::enter()
{
    m_age = 0;
    printf("[StartupScreen] enter\n");
}

void StartupScreen::leave()
{
    printf("[StartupScreen] leave\n");
}

bool StartupScreen::handleAction(Action action)
{
    // In Checkpoint A, any action advances past the splash
    (void)action;
    return false; // don't consume; let the stack handle it
}

void StartupScreen::update(Uint32 dt)
{
    m_age += dt;
}

void StartupScreen::render(SDL_Surface* fb)
{
    ui::SplashSpec spec;
    spec.headline = std::string("Version ") + VERSION_STR;
    spec.headlineColor = design::kTextSecondary;
    if (m_age > 500)
        spec.hints = {{ui::Key::A, "Continue"}};
    ui::splash(fb, spec);
}

} // namespace miyoofin
