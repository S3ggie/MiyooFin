#include "InputManager.hpp"
#include <cstdio>
#include <cstdlib>

namespace miyoofin {

int InputManager::dpadStateIndex(SDL_Scancode scancode, bool desktopInput)
{
    switch (scancode) {
    case static_cast<SDL_Scancode>(82): return 0;
    case static_cast<SDL_Scancode>(81): return 1;
    case static_cast<SDL_Scancode>(80): return 2;
    case static_cast<SDL_Scancode>(79): return 3;
    default: break;
    }
    if (desktopInput) {
        switch (scancode) {
        case SDL_SCANCODE_W: return 0;
        case SDL_SCANCODE_S: return 1;
        case SDL_SCANCODE_A: return 2;
        case SDL_SCANCODE_D: return 3;
        default: break;
        }
    }
    return -1;
}

Action InputManager::dpadAction(SDL_Scancode scancode, bool desktopInput)
{
    switch (scancode) {
    case static_cast<SDL_Scancode>(82): return Action::Up;
    case static_cast<SDL_Scancode>(81): return Action::Down;
    case static_cast<SDL_Scancode>(80): return Action::Left;
    case static_cast<SDL_Scancode>(79): return Action::Right;
    default: break;
    }
    if (desktopInput) {
        switch (scancode) {
        case SDL_SCANCODE_W: return Action::Up;
        case SDL_SCANCODE_S: return Action::Down;
        case SDL_SCANCODE_A: return Action::Left;
        case SDL_SCANCODE_D: return Action::Right;
        default: break;
        }
    }
    return Action::None;
}

InputManager::InputManager()
    : m_joystickIndex(-1)
{
    const char *desktopInput = std::getenv("MIYOOFIN_DESKTOP_INPUT");
    m_desktopInput = desktopInput && desktopInput[0] != '\0'
        && desktopInput[0] != '0';

    // Try to open the first available joystick / game controller
    if (SDL_NumJoysticks() > 0) {
        SDL_Joystick *joy = SDL_JoystickOpen(0);
        if (joy) {
            m_joystickIndex = 0;
            printf("[Input] Opened joystick: %s\n", SDL_JoystickName(joy));
        }
    }
}

std::vector<Action> InputManager::poll()
{
    std::vector<Action> actions;
    m_pointerClicks.clear();
    SDL_Event ev;

    while (SDL_PollEvent(&ev)) {
        Action action = Action::None;

        switch (ev.type) {
        case SDL_QUIT:
            actions.push_back(Action::Exit);
            addRawEvent(ev.type, false, 0, SDL_SCANCODE_UNKNOWN, 0, Action::Exit);
            break;

        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            bool down = (ev.type == SDL_KEYDOWN);
            SDL_Keycode kc = ev.key.keysym.sym;
            SDL_Scancode sc = ev.key.keysym.scancode;
            const int repeatIndex = dpadStateIndex(sc, m_desktopInput);

            if (repeatIndex >= 0) {
                action = dpadAction(sc, m_desktopInput);
                if (down) {
                    if (ev.key.repeat == 0 && beginDpadPress(
                            m_dpadRepeatStates[repeatIndex], action,
                            SDL_GetTicks()))
                        actions.push_back(action);
                } else {
                    endDpadPress(m_dpadRepeatStates[repeatIndex]);
                    action = Action::None;
                }

                addRawEvent(ev.type, down, kc, sc, 0, action);
                break;
            }

            // Confirmed Miyoo Mini Plus physical SDL scancodes.
            // These are the raw device scancodes reported by the
            // Miyoo SDL2 fork (verified on-device via diagnostics).
            if (down) {
                if (m_desktopInput &&
                    (kc == SDLK_RETURN || kc == SDLK_KP_ENTER)) {
                    action = Action::Confirm;
                } else if (m_desktopInput &&
                           (kc == SDLK_BACKSPACE || kc == SDLK_ESCAPE)) {
                    action = Action::Back;
                } else {
                    switch (sc) {
                    case 44:  action = Action::Confirm;     break;  // A
                    case 224: action = Action::Back;        break;  // B
                    case 225: action = Action::Search;      break;  // X
                    case 226: action = Action::ActionsMenu; break;  // Y
                    case 40:  action = Action::Settings;    break;  // START
                    case 228: action = Action::Menu;        break;  // SELECT
                    case 41:  action = Action::Exit;        break;  // MENU
                    case 8:   action = Action::PrevTab;     break;  // L
                    case 43:  action = Action::PrevPage;    break;  // L2
                    case 23:  action = Action::NextTab;     break;  // R
                    case 42:  action = Action::NextPage;    break;  // R2
                    default: break;
                    }
                }

                // Escape is the main "back" on host; also check for exit.
                if (kc == SDLK_ESCAPE && action == Action::None) {
                    action = Action::Back;
                }

                if (action != Action::None && ev.key.repeat == 0) {
                    actions.push_back(action);
                }
            }

            addRawEvent(ev.type, down, kc, sc, 0, action);
            break;
        }

        case SDL_MOUSEBUTTONDOWN:
            if (m_desktopInput && ev.button.button == SDL_BUTTON_LEFT) {
                m_pointerClicks.push_back({ev.button.x, ev.button.y});
            }
            addRawEvent(ev.type, true, 0, SDL_SCANCODE_UNKNOWN,
                        ev.button.button, Action::None);
            break;

        case SDL_JOYBUTTONDOWN:
        case SDL_JOYBUTTONUP: {
            bool down = (ev.type == SDL_JOYBUTTONDOWN);
            Uint8 btn = ev.jbutton.button;

            // Tentative: no mapping until tested on device
            if (down) {
                action = Action::Raw;
            }

            addRawEvent(ev.type, down, 0, SDL_SCANCODE_UNKNOWN, btn, action);
            if (action != Action::None) {
                actions.push_back(action);
            }
            break;
        }

        case SDL_JOYAXISMOTION: {
            // Log axis events but don't generate actions yet
            RawEvent re;
            re.timestamp = SDL_GetTicks();
            re.eventType = ev.type;
            re.isDown    = false;
            re.keycode   = 0;
            re.scancode  = SDL_SCANCODE_UNKNOWN;
            re.button    = 0;
            re.axis      = ev.jaxis.axis;
            re.axisValue = ev.jaxis.value;
            re.action    = Action::None;
            m_rawLog.push_back(re);
            if (m_rawLog.size() > MAX_LOG_ENTRIES)
                m_rawLog.erase(m_rawLog.begin());
            break;
        }

        default:
            break;
        }
    }

    const Uint32 now = SDL_GetTicks();
    for (auto &state : m_dpadRepeatStates) {
        if (takeDpadRepeat(state, now)) actions.push_back(state.action);
    }

    return actions;
}

void InputManager::addRawEvent(Uint32 type, bool isDown,
                                SDL_Keycode kc, SDL_Scancode sc,
                                Uint8 btn, Action action)
{
    RawEvent re;
    re.timestamp = SDL_GetTicks();
    re.eventType = type;
    re.isDown    = isDown;
    re.keycode   = kc;
    re.scancode  = sc;
    re.button    = btn;
    re.axis      = -1;
    re.axisValue = 0;
    re.action    = action;

    m_rawLog.push_back(re);
    if (m_rawLog.size() > MAX_LOG_ENTRIES)
        m_rawLog.erase(m_rawLog.begin());
}

void InputManager::suspend()
{
    resetDpadRepeatStates(m_dpadRepeatStates);
    // Joystick will be closed by SDL_QuitSubSystem, just reset our index
    m_joystickIndex = -1;
    printf("[Input] Suspended\n");
}

void InputManager::resume()
{
    resetDpadRepeatStates(m_dpadRepeatStates);
    m_joystickIndex = -1;
    // Reopen the first available joystick
    if (SDL_NumJoysticks() > 0) {
        SDL_Joystick *joy = SDL_JoystickOpen(0);
        if (joy) {
            m_joystickIndex = 0;
            printf("[Input] Reopened joystick: %s\n", SDL_JoystickName(joy));
        }
    }
    printf("[Input] Resumed\n");
}

} // namespace miyoofin
