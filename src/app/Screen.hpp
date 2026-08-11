#ifndef MIYOOFIN_SCREEN_HPP
#define MIYOOFIN_SCREEN_HPP

#include <SDL2/SDL.h>
#include <vector>
#include "../input/Action.hpp"

namespace miyoofin {

class ScreenStack;  // forward declaration

/// Abstract interface implemented by every screen.
class Screen {
public:
    virtual ~Screen() = default;

    /// Called when this screen becomes the active screen.
    virtual void enter() {}

    /// Called when this screen is being left (popped or replaced).
    virtual void leave() {}

    /// Process logical actions for this frame.
    /// @return true if the screen consumed the action (stops propagation).
    virtual bool handleAction(Action action) = 0;

    /// Fixed-step update (dt in milliseconds).
    virtual void update(Uint32 dt) = 0;

    /// Render the current state onto the software framebuffer surface.
    /// The framebuffer is 640x480 RGBA32, already cleared to the
    /// background colour before this call.
    virtual void render(SDL_Surface *fb) = 0;
    virtual const char *diagnosticName() const { return "Screen"; }

    /// Screens with cancellable, screen-owned workers can ask ScreenStack to
    /// retire them on its bounded cleanup worker.  leave() is still called on
    /// the UI thread first, so cancellation is signalled before destruction.
    virtual bool deferDestruction() const { return false; }

    /// Set the owning screen stack (called automatically on push).
    /// Screens can use this to push/pop screens from the stack.
    void setStack(ScreenStack *stack) { m_stack = stack; }

protected:
    ScreenStack *m_stack = nullptr;
};

} // namespace miyoofin

#endif // MIYOOFIN_SCREEN_HPP
