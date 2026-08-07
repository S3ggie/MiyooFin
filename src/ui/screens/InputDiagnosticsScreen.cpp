#include "InputDiagnosticsScreen.hpp"
#include "../Theme.hpp"
#include "../BitmapFont.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace miyoofin {

static const char *eventTypeName(Uint32 type)
{
    switch (type) {
    case SDL_KEYDOWN:           return "KEYDOWN";
    case SDL_KEYUP:             return "KEYUP  ";
    case SDL_JOYBUTTONDOWN:    return "JOY_DN ";
    case SDL_JOYBUTTONUP:      return "JOY_UP ";
    case SDL_JOYAXISMOTION:    return "JOY_AX ";
    case SDL_JOYHATMOTION:     return "JOY_HAT";
    case SDL_CONTROLLERBUTTONDOWN: return "CTRL_DN";
    case SDL_CONTROLLERBUTTONUP:   return "CTRL_UP";
    case SDL_QUIT:             return "QUIT   ";
    default:                   return "OTHER  ";
    }
}

InputDiagnosticsScreen::InputDiagnosticsScreen(InputManager *input)
    : m_input(input)
{
}

void InputDiagnosticsScreen::enter()
{
    printf("[InputDiagnosticsScreen] enter\n");
}

void InputDiagnosticsScreen::leave()
{
    printf("[InputDiagnosticsScreen] leave\n");
}

bool InputDiagnosticsScreen::handleAction(Action action)
{
    (void)action;
    // Exit handled by App directly
    return false;
}

void InputDiagnosticsScreen::update(Uint32 dt)
{
    (void)dt;
    // Copy raw events from the input manager
    if (m_input) {
        m_displayedEvents = m_input->rawLog();
    }
}

void InputDiagnosticsScreen::render(SDL_Surface *fb)
{
    // Title bar
    char title[64];
    std::snprintf(title, sizeof(title), "%s %s  --  Input Diagnostics",
                  APP_NAME, VERSION_STR);
    BitmapFont::drawString(fb, 4, 4, title,
                           Theme::ACCENT_R, Theme::ACCENT_G, Theme::ACCENT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // Column headers
    int headerY = 4 + BitmapFont::GLYPH_H + 4;
    BitmapFont::drawString(fb, 4, headerY,
                           "Event      Type   Keycode  ScanCode  Btn  Ax  AxVal  Action",
                           Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
    BitmapFont::drawString(fb, 4, headerY + BitmapFont::GLYPH_H,
                           "-------------------------------------------------------------",
                           Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);

    // Event list  (scrolling, newest at bottom)
    int listY = headerY + 2 * BitmapFont::GLYPH_H + 2;
    int maxRows = (fb->h - listY - 8) / (BitmapFont::GLYPH_H + 1);
    int startIdx = 0;
    if ((int)m_displayedEvents.size() > maxRows) {
        startIdx = (int)m_displayedEvents.size() - maxRows;
    }

    for (int i = startIdx; i < (int)m_displayedEvents.size(); ++i) {
        const RawEvent &e = m_displayedEvents[i];
        char line[128];

        // Format:  EventType  Dn/Up  Keycode  Scancode  Btn  Ax  AxVal  Action
        if (e.eventType == SDL_JOYAXISMOTION) {
            std::snprintf(line, sizeof(line),
                          "%s          |      |    %3d  |%5d | %s",
                          eventTypeName(e.eventType),
                          (int)e.axis, (int)e.axisValue,
                          actionName(e.action));
        } else {
            std::snprintf(line, sizeof(line),
                          "%s %s  %6d  %5d    %3d  |    |      %s",
                          eventTypeName(e.eventType),
                          e.isDown ? "DOWN" : "UP  ",
                          (int)e.keycode, (int)e.scancode,
                          (int)e.button,
                          actionName(e.action));
        }

        // Dim older events
        int age = (int)m_displayedEvents.size() - 1 - i;
        Uint8 r = Theme::TEXT_R - (Uint8)(age * 2);
        Uint8 g = Theme::TEXT_G - (Uint8)(age * 2);
        Uint8 b = Theme::TEXT_B - (Uint8)(age * 2);
        if (r < 80) r = 80;
        if (g < 80) g = 80;
        if (b < 80) b = 80;

        BitmapFont::drawString(fb, 4, listY, line,
                               r, g, b,
                               Theme::BG_R, Theme::BG_G, Theme::BG_B);
        listY += BitmapFont::GLYPH_H + 1;
    }

    // Bottom hint
    int hintY = fb->h - BitmapFont::GLYPH_H - 4;
    BitmapFont::drawString(fb, 4, hintY,
                           "MENU / F1 = Exit   |   Raw event display for mapping",
                           Theme::TEXT_R, Theme::TEXT_G, Theme::TEXT_B,
                           Theme::BG_R, Theme::BG_G, Theme::BG_B);
}

} // namespace miyoofin