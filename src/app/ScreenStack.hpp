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

private:
    std::vector<std::unique_ptr<Screen>> m_stack;
};

} // namespace miyoofin

#endif // MIYOOFIN_SCREEN_STACK_HPP