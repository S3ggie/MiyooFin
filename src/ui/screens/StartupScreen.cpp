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
    return false;  // don't consume; let the stack handle it
}

void StartupScreen::update(Uint32 dt)
{
    m_age += dt;
}

void StartupScreen::render(SDL_Surface *fb)
{
    const char *title = APP_NAME;
    const char *ver   = VERSION_STR;

    // Draw title centered
    int titleLen = (int)::strlen(title);
    int titleX   = (fb->w - titleLen * BitmapFont::GLYPH_W) / 2;
    int titleY   = fb->h / 3;

    BitmapFont::drawString(fb, titleX, titleY, title,
                           Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // Draw version below
    int verLen = (int)::strlen(ver);
    int verX   = (fb->w - verLen * BitmapFont::GLYPH_W) / 2;
    int verY   = titleY + BitmapFont::GLYPH_H + 8;

    BitmapFont::drawString(fb, verX, verY, ver,
                           Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // Draw "Press any button" after 500ms
    if (m_age > 500) {
        const char *hint = "Press any button...";
        int hintLen = (int)::strlen(hint);
        int hintX   = (fb->w - hintLen * BitmapFont::GLYPH_W) / 2;
        int hintY   = verY + BitmapFont::GLYPH_H + 20;
        BitmapFont::drawString(fb, hintX, hintY, hint,
                               Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                               Theme::BG_R, Theme::BG_G, Theme::BG_B);
    }
}

} // namespace miyoofin