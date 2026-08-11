#include "ScreenStack.hpp"
#include "UiDiagnostics.hpp"
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

namespace miyoofin {

namespace {
const char *pushDiagnosticName(const Screen *screen)
{
    if(!screen)return "ScreenStack::push -> null";
    const char *name=screen->diagnosticName();
    if(!std::strcmp(name,"MovieDetailsScreen"))return "ScreenStack::push -> MovieDetailsScreen";
    if(!std::strcmp(name,"SeriesScreen"))return "ScreenStack::push -> SeriesScreen";
    if(!std::strcmp(name,"EpisodeBrowserScreen"))return "ScreenStack::push -> EpisodeBrowserScreen";
    if(!std::strcmp(name,"HomeScreen"))return "ScreenStack::push -> HomeScreen";
    return "ScreenStack::push -> Screen";
}
}

struct ScreenStack::RetirementQueue {
    RetirementQueue() : worker(&RetirementQueue::run, this) {}
    ~RetirementQueue()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    void push(std::unique_ptr<Screen> screen)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending.push_back(std::move(screen));
        }
        wake.notify_one();
    }

    void run()
    {
        for (;;) {
            std::unique_ptr<Screen> screen;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return stopping || !pending.empty(); });
                if (pending.empty()) {
                    if (stopping) return;
                    continue;
                }
                screen = std::move(pending.front());
                pending.pop_front();
            }
            // Destruction joins the screen workers here, never on SDL's
            // event/update path.  The screen remains alive until all workers
            // that reference its state have stopped.
            screen.reset();
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::unique_ptr<Screen>> pending;
    bool stopping = false;
    std::thread worker;
};

ScreenStack::ScreenStack() = default;
ScreenStack::~ScreenStack() = default;

void ScreenStack::retire(std::unique_ptr<Screen> screen)
{
    if (!m_retirement) m_retirement.reset(new RetirementQueue());
    m_retirement->push(std::move(screen));
}

void ScreenStack::push(std::unique_ptr<Screen> screen)
{
    UiDiagnostics::Scope scope(pushDiagnosticName(screen.get()));
    if (screen) {
        uiDiagnostics().setScreen(screen->diagnosticName());
        screen->setStack(this);
        screen->enter();
        m_stack.push_back(std::move(screen));
        uiDiagnostics().event("screen pushed");
    }
}

bool ScreenStack::pop()
{
    UiDiagnostics::Scope scope("ScreenStack::pop");
    if (m_stack.size() <= 1)
        return false;
    {
        UiDiagnostics::Scope leaveScope("ScreenStack::signalLeave");
        m_stack.back()->leave();
    }
    std::unique_ptr<Screen> removed = std::move(m_stack.back());
    m_stack.pop_back();
    if (removed->deferDestruction()) {
        UiDiagnostics::Scope retireScope("ScreenStack::retireScreen");
        retire(std::move(removed));
    } else {
        removed.reset();
    }
    {
        UiDiagnostics::Scope enterScope("ScreenStack::enterPrevious");
        m_stack.back()->enter();  // re-activate the new top
    }
    uiDiagnostics().event("screen popped");
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
