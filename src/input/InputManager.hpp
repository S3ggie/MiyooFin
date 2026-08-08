#ifndef MIYOOFIN_INPUT_MANAGER_HPP
#define MIYOOFIN_INPUT_MANAGER_HPP

#include <SDL2/SDL.h>
#include <vector>
#include <string>
#include "Action.hpp"

namespace miyoofin {

/// Stores a single input event for the diagnostics log.
struct RawEvent {
    Uint32      timestamp;   // SDL_GetTicks() when captured
    Uint32      eventType;   // SDL event type
    bool        isDown;      // true = key down / button down, false = up
    SDL_Keycode keycode;     // SDL virtual key code (0 for joystick)
    SDL_Scancode scancode;   // SDL physical scancode (0 for joystick)
    Uint8       button;      // controller / joystick button index (0 for keyboard)
    Sint32      axis;        // axis index (for joystick axis events)
    Sint16      axisValue;   // axis value
    Action      action;      // the tentative mapped action
};

/// InputManager reads SDL events and converts them to logical actions.
/// It also keeps a log of raw events for the diagnostics screen.
class InputManager {
public:
    InputManager();

    /// Poll all pending SDL events, store raw log entries,
    /// and return a list of logical actions for this frame.
    std::vector<Action> poll();

    /// Suspend input (close joystick). Called before SDL_QuitSubSystem.
    void suspend();

    /// Resume input (reopen joystick). Called after SDL_InitSubSystem.
    void resume();

    /// Access the raw event log (most recent events).
    const std::vector<RawEvent>& rawLog() const { return m_rawLog; }

    /// Maximum number of raw events kept in the scrolling log.
    static constexpr int MAX_LOG_ENTRIES = 50;

private:
    std::vector<RawEvent> m_rawLog;
    int m_joystickIndex;          // -1 if none opened

    void addRawEvent(Uint32 type, bool isDown,
                     SDL_Keycode kc, SDL_Scancode sc,
                     Uint8 btn, Action action);
};

} // namespace miyoofin

#endif // MIYOOFIN_INPUT_MANAGER_HPP