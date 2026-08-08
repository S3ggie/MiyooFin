#include "InputManager.hpp"
#include <cstdio>

namespace miyoofin {

InputManager::InputManager()
    : m_joystickIndex(-1)
{
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

            // Confirmed Miyoo Mini Plus physical SDL scancodes.
            // These are the raw device scancodes reported by the
            // Miyoo SDL2 fork (verified on-device via diagnostics).
            if (down) {
                switch (sc) {
                case 82:  action = Action::Up;          break;  // Up
                case 81:  action = Action::Down;        break;  // Down
                case 80:  action = Action::Left;        break;  // Left
                case 79:  action = Action::Right;       break;  // Right
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

                // Escape is the main "back" on host; also check for exit
                if (kc == SDLK_ESCAPE && action == Action::None) {
                    action = Action::Back;
                }

                if (action != Action::None) {
                    actions.push_back(action);
                }
            }

            addRawEvent(ev.type, down, kc, sc, 0, action);
            break;
        }

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
    // Joystick will be closed by SDL_QuitSubSystem, just reset our index
    m_joystickIndex = -1;
    printf("[Input] Suspended\n");
}

void InputManager::resume()
{
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