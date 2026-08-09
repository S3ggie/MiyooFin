#ifndef MIYOOFIN_INPUT_MANAGER_HPP
#define MIYOOFIN_INPUT_MANAGER_HPP

#include <SDL2/SDL.h>
#include <array>
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
    static constexpr Uint32 DPAD_REPEAT_INITIAL_DELAY_MS = 300;
    static constexpr Uint32 DPAD_REPEAT_INTERVAL_MS = 90;

    struct DpadRepeatState {
        bool held = false;
        Action action = Action::None;
        Uint32 nextRepeatAt = 0;
    };

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

    // Pure repeat-state helpers exposed for deterministic host tests.
    static bool isDpadRepeatAction(Action action) {
        return action == Action::Up || action == Action::Down ||
               action == Action::Left || action == Action::Right;
    }
    static bool beginDpadPress(DpadRepeatState &state, Action action,
                               Uint32 now) {
        if (!isDpadRepeatAction(action) || state.held) return false;
        state.held = true;
        state.action = action;
        state.nextRepeatAt = now + DPAD_REPEAT_INITIAL_DELAY_MS;
        return true;
    }
    static void endDpadPress(DpadRepeatState &state) {
        state.held = false;
        state.nextRepeatAt = 0;
    }
    static bool takeDpadRepeat(DpadRepeatState &state, Uint32 now) {
        if (!state.held ||
            static_cast<Sint32>(now - state.nextRepeatAt) < 0)
            return false;
        const Uint32 elapsed = now - state.nextRepeatAt;
        state.nextRepeatAt +=
            (elapsed / DPAD_REPEAT_INTERVAL_MS + 1) *
            DPAD_REPEAT_INTERVAL_MS;
        return true;
    }
    static void resetDpadRepeatStates(
        std::array<DpadRepeatState, 4> &states) {
        for (auto &state : states) state = {};
    }

private:
    std::vector<RawEvent> m_rawLog;
    int m_joystickIndex;          // -1 if none opened
    std::array<DpadRepeatState, 4> m_dpadRepeatStates;

    static int dpadStateIndex(SDL_Scancode scancode);
    static Action dpadAction(SDL_Scancode scancode);

    void addRawEvent(Uint32 type, bool isDown,
                     SDL_Keycode kc, SDL_Scancode sc,
                     Uint8 btn, Action action);
};

} // namespace miyoofin

#endif // MIYOOFIN_INPUT_MANAGER_HPP
