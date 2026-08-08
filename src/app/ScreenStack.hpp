#ifndef MIYOOFIN_SCREEN_STACK_HPP
#define MIYOOFIN_SCREEN_STACK_HPP

#include <vector>
#include <memory>
#include "Screen.hpp"

namespace miyoofin {

/// Manages a stack of screens.
/// Only the topmost screen receives events, updates, and renders.
class ScreenStack {
public:
    ScreenStack() = default;

    /// Push a new screen onto the stack (it becomes active).
    void push(std::unique_ptr<Screen> screen);

    /// Pop the top screen. Returns false if stack would be empty.
    bool pop();

    /// Pop until the stack has exactly one screen (the root).
    void popToRoot();

    /// Access the active (top) screen.
    Screen *top() const;

    /// Number of screens on the stack.
    int size() const { return static_cast<int>(m_stack.size()); }

    /// True if the stack is empty.
    bool empty() const { return m_stack.empty(); }

    /// Request in-process external playback.
    /// Called by screens when a playback-request.txt has been written.
    /// Sets a flag consumed by App's main loop, which then suspends
    /// SDL, spawns the playback runner, waits, and resumes — without
    /// destroying the ScreenStack.
    void requestExternalPlayback() { m_externalPlayback = true; }

    /// Check (and consume) the external playback flag.
    bool pollExternalPlayback() {
        bool v = m_externalPlayback;
        m_externalPlayback = false;
        return v;
    }

private:
    std::vector<std::unique_ptr<Screen>> m_stack;
    bool m_externalPlayback = false;
};

} // namespace miyoofin

#endif // MIYOOFIN_SCREEN_STACK_HPP