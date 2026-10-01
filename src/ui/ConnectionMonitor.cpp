#include "ConnectionMonitor.hpp"

#include "../diagnostics/UiDiagnostics.hpp"

#include <cstdio>

namespace miyoofin {

static const char* linkName(LinkStatus status)
{
    switch (status) {
    case LinkStatus::Checking:
        return "checking";
    case LinkStatus::Connected:
        return "connected";
    case LinkStatus::Offline:
        return "offline";
    case LinkStatus::SessionExpired:
        return "session_expired";
    case LinkStatus::OfflineMode:
        return "offline_mode";
    }
    return "unknown";
}

ConnectionMonitor::~ConnectionMonitor()
{
    // The probe observes the cancel token (HTTP aborts promptly), so this join
    // cannot wait out a network timeout.
    m_worker.cancel();
    m_worker.join();
}

void ConnectionMonitor::setOfflineMode(bool on)
{
    if (m_offlineMode == on)
        return;
    m_offlineMode = on;
    if (!on) {
        m_status = LinkStatus::Checking;
        m_failStreak = 0;
        m_pokePending = true; // probe right away instead of waiting out an interval
    }
}

void ConnectionMonitor::update(unsigned dtMs)
{
    if (m_worker.reap()) {
        const LinkStatus before = status();
        m_sinceProbeMs = 0;
        if (m_result == LinkStatus::Connected) {
            m_status = LinkStatus::Connected;
            m_failStreak = 0;
        } else if (m_result == LinkStatus::SessionExpired) {
            m_status = LinkStatus::SessionExpired; // definitive: shown at once
            m_failStreak = 0;
        } else {
            // One failure after a good probe keeps "connected"; a second in a
            // row (or any failure before the first success) is offline.
            ++m_failStreak;
            if (m_status != LinkStatus::Connected || m_failStreak >= 2)
                m_status = LinkStatus::Offline;
        }
        if (status() != before) {
            char line[64];
            std::snprintf(line, sizeof(line), "[Link] status=%s", linkName(status()));
            std::printf("%s\n", line);
            uiDiagnostics().log(line);
        }
    }
    if (m_offlineMode)
        return; // nothing to probe; any in-flight result above was still reaped

    m_sinceProbeMs += dtMs;
    const unsigned interval =
        m_status == LinkStatus::Connected && m_failStreak == 0 ? m_okIntervalMs : m_retryIntervalMs;
    if (m_worker.busy() || !m_probe)
        return;
    if (!m_started || m_pokePending || m_sinceProbeMs >= interval) {
        m_started = true;
        m_pokePending = false;
        m_sinceProbeMs = 0;
        m_worker.start([this](const CancelToken& cancel) { m_result = m_probe(cancel); });
    }
}

} // namespace miyoofin
