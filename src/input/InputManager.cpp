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

            // Tentative host-side mapping for development.
            // These approximate the Miyoo layout and will be
            // adjusted once the real device mapping is known.
            if (down) {
                switch (kc) {
                case SDLK_UP:       action = Action::Up;        break;
                case SDLK_DOWN:     action = Action::Down;      break;
                case SDLK_LEFT:     action = Action::Left;      break;
                case SDLK_RIGHT:    action = Action::Right;     break;
                case SDLK_RETURN:
                case SDLK_SPACE:    action = Action::Confirm;   break;
                case SDLK_BACKSPACE: action = Action::Back;     break;
                case SDLK_TAB:      action = Action::NextTab;   break;
                case SDLK_f:        action = Action::Search;    break;
                case SDLK_g:        action = Action::ActionsMenu; break;
                case SDLK_PAGEUP:   action = Action::PrevPage;  break;
                case SDLK_PAGEDOWN: action = Action::NextPage;  break;
                case SDLK_ESCAPE:
                    // Escape handled below switch (defaults to Back)
                    break;
                case SDLK_LSHIFT:
                case SDLK_RSHIFT:   action = Action::Settings;  break;
                case SDLK_LCTRL:
                case SDLK_RCTRL:    action = Action::Menu;      break;
                case SDLK_F1:       action = Action::Exit;       break;
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

} // namespace miyoofin