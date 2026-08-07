#include "ScreenStack.hpp"

namespace miyoofin {

void ScreenStack::push(std::unique_ptr<Screen> screen)
{
    if (screen) {
        screen->setStack(this);
        screen->enter();
        m_stack.push_back(std::move(screen));
    }
}

bool ScreenStack::pop()
{
    if (m_stack.size() <= 1)
        return false;
    m_stack.back()->leave();
    m_stack.pop_back();
    m_stack.back()->enter();  // re-activate the new top
    return true;
}

void ScreenStack::popToRoot()
{
    while (m_stack.size() > 1)
        pop();
}

Screen *ScreenStack::top() const
{
    if (m_stack.empty())
        return nullptr;
    return m_stack.back().get();
}

} // namespace miyoofin