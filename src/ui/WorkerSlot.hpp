#ifndef MIYOOFIN_WORKER_SLOT_HPP
#define MIYOOFIN_WORKER_SLOT_HPP

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace miyoofin {

using CancelToken = std::shared_ptr<std::atomic_bool>;

// One owner-scoped background worker with at most one run in flight.
//
// All members are owner-thread only (the UI thread), except that the worker
// function receives its own CancelToken and may read it from the worker
// thread. The slot never blocks on start/reap/cancel; only join() and the
// destructor block, so callers must cancel and join in leave()/teardown
// paths that are already allowed to wait.
class WorkerSlot
{
  public:
    WorkerSlot() = default;
    WorkerSlot(const WorkerSlot&) = delete;
    WorkerSlot& operator=(const WorkerSlot&) = delete;

    // Backstop only: owners should cancel()+join() explicitly first.
    ~WorkerSlot()
    {
        cancel();
        join();
    }

    // Starts fn on a fresh token. Refused (false) while a run is still
    // executing; a finished-but-unreaped run is reclaimed first.
    bool start(std::function<void(const CancelToken&)> fn)
    {
        if (m_thread.joinable()) {
            if (!m_finished.load(std::memory_order_acquire))
                return false;
            m_thread.join();
        }
        m_token = std::make_shared<std::atomic_bool>(false);
        m_finished.store(false, std::memory_order_relaxed);
        const CancelToken token = m_token;
        m_thread = std::thread([this, token, fn = std::move(fn)]() mutable {
            fn(token);
            fn = nullptr; // release captures before publishing completion
            m_finished.store(true, std::memory_order_release);
        });
        return true;
    }

    // Joins and returns true once per run, only after the worker finished.
    // The join is the happens-before edge for any result fields it wrote.
    bool reap()
    {
        if (!m_thread.joinable() || !m_finished.load(std::memory_order_acquire))
            return false;
        m_thread.join();
        return true;
    }

    // Started and not yet reaped.
    bool busy() const
    {
        return m_thread.joinable();
    }

    // Requests cooperative cancellation of the current run. Never blocks.
    void cancel() noexcept
    {
        if (m_token)
            m_token->store(true, std::memory_order_relaxed);
    }

    // BLOCKING: destructor/retirement paths only.
    void join()
    {
        if (m_thread.joinable())
            m_thread.join();
    }

  private:
    std::thread m_thread;
    CancelToken m_token;
    std::atomic_bool m_finished{true};
};

} // namespace miyoofin

#endif
