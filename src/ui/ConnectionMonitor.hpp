#ifndef MIYOOFIN_CONNECTION_MONITOR_HPP
#define MIYOOFIN_CONNECTION_MONITOR_HPP

#include "WorkerSlot.hpp"

#include <functional>
#include <utility>

namespace miyoofin {

enum class LinkStatus
{
    Checking,       // first probe has not finished yet
    Connected,      // server reachable and the session is accepted
    Offline,        // server unreachable
    SessionExpired, // server reachable but it rejected our token (HTTP 401/403)
    OfflineMode     // the user switched to offline mode; not probing
};

// UI-thread view of "are we really connected to the server?". A probe runs on
// a WorkerSlot (never on the UI thread): every kOkIntervalMs while connected
// and every kRetryIntervalMs otherwise, so recovery shows up quickly. Leaving
// Connected for Offline needs two failed probes in a row so one dropped packet
// does not flicker the UI; a rejected session shows immediately. The probe
// must honour its cancel token so destroying the monitor never waits on a
// network timeout.
class ConnectionMonitor
{
  public:
    // Runs on the worker; returns Connected, Offline or SessionExpired.
    using Probe = std::function<LinkStatus(const CancelToken&)>;

    static constexpr unsigned kOkIntervalMs = 20000;
    static constexpr unsigned kRetryIntervalMs = 5000;

    explicit ConnectionMonitor(Probe probe, unsigned okIntervalMs = kOkIntervalMs,
                               unsigned retryIntervalMs = kRetryIntervalMs)
        : m_probe(std::move(probe)), m_okIntervalMs(okIntervalMs),
          m_retryIntervalMs(retryIntervalMs)
    {}
    ~ConnectionMonitor();

    void update(unsigned dtMs);

    LinkStatus status() const
    {
        return m_offlineMode ? LinkStatus::OfflineMode : m_status;
    }

    // Offline mode reports OfflineMode without probing; leaving it re-checks now.
    void setOfflineMode(bool on);

    // Probe again as soon as possible (for example after the network changed).
    void pokeNow()
    {
        m_pokePending = true;
    }

  private:
    Probe m_probe;
    unsigned m_okIntervalMs;
    unsigned m_retryIntervalMs;
    LinkStatus m_status = LinkStatus::Checking;
    bool m_offlineMode = false;
    bool m_pokePending = false;
    bool m_started = false;
    int m_failStreak = 0;
    unsigned m_sinceProbeMs = 0;
    LinkStatus m_result = LinkStatus::Offline; // written by the worker, read after reap()
    WorkerSlot m_worker;                       // last member: joined before the fields above
};

} // namespace miyoofin

#endif // MIYOOFIN_CONNECTION_MONITOR_HPP
