#ifndef MIYOOFIN_UI_DIAGNOSTICS_HPP
#define MIYOOFIN_UI_DIAGNOSTICS_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include "../diagnostics/TelemetryIds.hpp"

namespace miyoofin {

// Kept deliberately independent from SDL so its timing/ring behaviour is host-testable.
class UiDiagnostics {
public:
    static constexpr uint64_t STALL_MS = 500;
    static constexpr uint64_t SLOW_MS = 100;
    static constexpr size_t EVENT_CAPACITY = 48;
    struct Watchdog { uint64_t seen=0; bool stalled=false;
        // Returns 1 for begin, 2 for end, 0 otherwise.  `suspended` suppresses and resets a pending stall.
        int poll(uint64_t heartbeat, uint64_t now, bool suspended, uint64_t &duration);
    };
    class Scope {
    public:
        // Worker scopes still emit slow-operation timing, but must not replace
        // the UI scope watched by the stall detector.
        explicit Scope(const char *name);
        Scope(const char *name, bool trackUiScope);
        ~Scope();
    private:
        const char *m_name;
        const char *m_previous;
        uint64_t m_start;
        bool m_trackUiScope;
    };

    UiDiagnostics();
    ~UiDiagnostics();
    void start(const std::string &path = "/mnt/SDCARD/App/MiyooFin/ui-stall.log");
    void stop();
    void heartbeat();
    static UiPhaseId phaseIdFromDiagnosticName(const char *phase) noexcept;
    static UiScopeId scopeIdFromDiagnosticName(const char *scope) noexcept;
    void setPhase(const char *phase) { m_phase.store(phase, std::memory_order_relaxed); }
    const char *phaseName() const noexcept { return m_phase.load(std::memory_order_relaxed); }
    void setScreen(const char *screen) { m_screen.store(screen, std::memory_order_relaxed); }
    const char *screenName() const noexcept { return m_screen.load(std::memory_order_relaxed); }
    void setTab(const char *tab) { m_tab.store(tab, std::memory_order_relaxed); }
    const char *tabName() const noexcept { return m_tab.load(std::memory_order_relaxed); }
    void setLastAction(const char *action) { m_action.store(action, std::memory_order_relaxed); }
    const char *actionName() const noexcept { return m_action.load(std::memory_order_relaxed); }
    void setScope(const char *scope) { m_scope.store(scope, std::memory_order_relaxed); }
    const char *scopeName() const noexcept { return m_scope.load(std::memory_order_relaxed); }
    const char *exchangeScope(const char *scope) { return m_scope.exchange(scope, std::memory_order_relaxed); }
    void setSuspended(bool suspended) { m_suspended.store(suspended, std::memory_order_relaxed); }
    void setWorker(const char *worker, const char *state);
    uint16_t activeWorkerMask() const noexcept
    { return m_activeWorkerMask.load(std::memory_order_relaxed); }
    void event(const char *message);
    std::vector<std::string> recentEvents() const;
    static uint64_t monotonicMs();
private:
    void watchdogLoop(); void writeLine(const std::string &line); void slow(const char *name, uint64_t elapsed);
    std::atomic<uint64_t> m_heartbeat{0}; std::atomic<bool> m_stop{false}, m_suspended{false};
    std::atomic<uint16_t> m_activeWorkerMask{0};
    std::atomic<const char *> m_phase{"idle/frame boundary"}, m_screen{"none"}, m_tab{"n/a"}, m_action{"None"}, m_scope{""};
    std::atomic<const char *> m_library{"idle"}, m_hierarchy{"idle"}, m_artwork{"idle"}, m_download{"idle"};
    std::thread m_thread; std::string m_path; mutable std::mutex m_eventsMutex; std::vector<std::string> m_events;
    std::mutex m_pendingMutex; std::vector<std::string> m_pendingLogs;
};

UiDiagnostics &uiDiagnostics();
}
#endif
