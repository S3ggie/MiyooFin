#include "InputDiagnosticsScreen.hpp"
#include "../Theme.hpp"
#include "../UiKit.hpp"
#include "../BitmapFont.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace miyoofin {

static const char* eventTypeName(Uint32 type)
{
    switch (type) {
    case SDL_KEYDOWN:
        return "KEYDOWN";
    case SDL_KEYUP:
        return "KEYUP  ";
    case SDL_JOYBUTTONDOWN:
        return "JOY_DN ";
    case SDL_JOYBUTTONUP:
        return "JOY_UP ";
    case SDL_JOYAXISMOTION:
        return "JOY_AX ";
    case SDL_JOYHATMOTION:
        return "JOY_HAT";
    case SDL_CONTROLLERBUTTONDOWN:
        return "CTRL_DN";
    case SDL_CONTROLLERBUTTONUP:
        return "CTRL_UP";
    case SDL_QUIT:
        return "QUIT   ";
    default:
        return "OTHER  ";
    }
}

InputDiagnosticsScreen::InputDiagnosticsScreen(InputManager* input) : m_input(input) {}

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

void InputDiagnosticsScreen::render(SDL_Surface* fb)
{
    namespace d = design;
    ui::fill(fb, 0, 0, d::kScreenW, d::kScreenH, d::kCanvas);
    ui::HeaderSpec header;
    header.title = std::string("Input diagnostics  ") + VERSION_STR;
    header.showStatus = false;
    ui::header(fb, header);

    // Column headers
    const int headerY = d::kHeaderH + 6;
    ui::text(fb, 8, headerY, "Event      Type   Keycode  ScanCode  Btn  Ax  AxVal  Action",
             d::kAccentHi);
    ui::fill(fb, 8, headerY + BitmapFont::GLYPH_H + 2, d::kScreenW - 16, 1, d::kDivider);

    // Event list (scrolling, newest at bottom)
    int listY = headerY + BitmapFont::GLYPH_H + 6;
    const int maxRows = (d::kScreenH - d::kFooterH - 4 - listY) / (BitmapFont::GLYPH_H + 1);
    int startIdx = 0;
    if ((int)m_displayedEvents.size() > maxRows)
        startIdx = (int)m_displayedEvents.size() - maxRows;

    for (int i = startIdx; i < (int)m_displayedEvents.size(); ++i) {
        const RawEvent& e = m_displayedEvents[i];
        char line[128];

        // Format:  EventType  Dn/Up  Keycode  Scancode  Btn  Ax  AxVal  Action
        if (e.eventType == SDL_JOYAXISMOTION) {
            std::snprintf(line, sizeof(line), "%s          |      |    %3d  |%5d | %s",
                          eventTypeName(e.eventType), (int)e.axis, (int)e.axisValue,
                          actionName(e.action));
        } else {
            std::snprintf(line, sizeof(line), "%s %s  %6d  %5d    %3d  |    |      %s",
                          eventTypeName(e.eventType), e.isDown ? "DOWN" : "UP  ", (int)e.keycode,
                          (int)e.scancode, (int)e.button, actionName(e.action));
        }

        // Dim older events
        const int age = (int)m_displayedEvents.size() - 1 - i;
        const d::Rgb shade = ui::mix(d::kText, d::kTextMuted, std::min(100, age * 6));
        ui::text(fb, 8, listY, line, shade);
        listY += BitmapFont::GLYPH_H + 1;
    }

    ui::FooterSpec footer;
    footer.showLink = false;
    footer.hints = {{ui::Key::Menu, "Exit"}};
    footer.note = "Raw event display for mapping";
    ui::footer(fb, footer);
}

} // namespace miyoofin
