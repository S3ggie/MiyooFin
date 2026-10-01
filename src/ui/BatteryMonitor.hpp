#ifndef MIYOOFIN_BATTERY_MONITOR_HPP
#define MIYOOFIN_BATTERY_MONITOR_HPP

#include "WorkerSlot.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace miyoofin {

// UI-thread battery state for the Home header icon.
//
// Level: OnionOS's batmon daemon publishes the percentage in /tmp/percBat (the
// Mini Plus has no standard power_supply node); that tiny tmpfs file is read at
// most every kRefreshMs. Where it does not exist (desktop builds) the level
// stays unknown (-1) and the icon is drawn empty rather than pretending to be
// full.
//
// Charging: the only source on the Mini Plus is the power chip query
// /customer/app/axp_test (JSON with a "charging" field), which needs root; the
// app runs as root under Onion. It runs on a WorkerSlot every kChargingRefreshMs
// via fork/exec with a hard timeout and kill, so a stuck chip query can never
// block the UI thread or app exit. When it is unavailable (not found, not
// root) charging stays false and nothing is drawn.
class BatteryMonitor
{
  public:
    static constexpr unsigned kRefreshMs = 30000;
    // Charging is polled once per second so plugging in shows up promptly; each
    // probe is one short-lived process.
    static constexpr unsigned kChargingRefreshMs = 1000;
    // A failed probe keeps the last known state; this many in a row means unknown.
    static constexpr int kChargingUnknownLimit = 5;

    explicit BatteryMonitor(std::string percentPath = "/tmp/percBat",
                            std::string axpPath = "/customer/app/axp_test",
                            unsigned probeTimeoutMs = 2000)
        : m_path(std::move(percentPath)), m_axpPath(std::move(axpPath)),
          m_probeTimeoutMs(probeTimeoutMs)
    {}
    ~BatteryMonitor();

    // Call every frame with the elapsed time. Reads the level on the first call
    // and then once per kRefreshMs; starts/reaps the charging probe.
    void update(unsigned dtMs);

    // 0..100, or -1 when unknown.
    int percent() const
    {
        return m_percent;
    }

    // True only when the power chip reported that the battery is charging.
    bool charging() const
    {
        return m_charging == 1;
    }

    // "48\n" -> 48. Leading whitespace is skipped; anything that is not a
    // whole number in 0..100 is invalid (-1).
    static int parsePercent(const std::string& content);

    // Extracts "charging":N from axp_test output the way OnionOS's batmon does
    // (sscanf %d, then non-zero means charging): 0 or 1, or -1 when the key is
    // missing or has no number (for example an I2C error). The raw number is
    // returned through `rawOut` when given, since the chip may report values
    // other than 0/1 while charging.
    static int parseCharging(const std::string& output, int* rawOut = nullptr);

    // Width of the fill inside an icon `innerWidth` pixels wide: 0 when unknown
    // or empty, at least 1 pixel for any positive level, innerWidth when full.
    static int fillWidth(int percent, int innerWidth);

    struct Color
    {
        std::uint8_t r, g, b;
    };
    // Green above 40%, amber 16-40%, red at 15% and below.
    static Color levelColor(int percent);

  private:
    // Worker body: runs the probe program (bounded by m_probeTimeoutMs, killed
    // on timeout or cancellation) and stores its stdout.
    void runProbe(const CancelToken& cancel);

    std::string m_path;
    std::string m_axpPath;
    unsigned m_probeTimeoutMs;
    int m_percent = -1;
    int m_charging = -1; // -1 unknown, 0 no, 1 yes
    int m_chargingRaw = -1;
    int m_unknownStreak = 0;
    unsigned m_sinceReadMs = 0;
    bool m_hasRead = false;
    unsigned m_sinceProbeMs = 0;
    bool m_neverProbed = true;
    bool m_probeUnavailable = false;
    int m_loggedCharging = -2; // last (state, raw) logged, to log only changes
    int m_loggedRaw = -2;
    // Written by the worker, read by the UI thread only after reap().
    std::string m_probeOutput;
    bool m_probeExecFailed = false;
    WorkerSlot m_probe; // last member: destroyed (joined) before the fields above
};

} // namespace miyoofin

#endif // MIYOOFIN_BATTERY_MONITOR_HPP
