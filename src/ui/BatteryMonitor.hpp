#ifndef MIYOOFIN_BATTERY_MONITOR_HPP
#define MIYOOFIN_BATTERY_MONITOR_HPP

#include <cstdint>
#include <string>

namespace miyoofin {

// UI-thread battery level for the Home header icon. OnionOS's batmon daemon
// publishes the percentage in /tmp/percBat (the Mini Plus has no standard
// power_supply node); this reads that tiny tmpfs file at most every
// kRefreshMs. Where the file does not exist (desktop builds) the level stays
// unknown (-1) and the icon is drawn empty rather than pretending to be full.
class BatteryMonitor
{
  public:
    static constexpr unsigned kRefreshMs = 30000;

    explicit BatteryMonitor(std::string path = "/tmp/percBat") : m_path(std::move(path)) {}

    // Call every frame with the elapsed time; reads on the first call and then
    // once per kRefreshMs.
    void update(unsigned dtMs);

    // 0..100, or -1 when unknown.
    int percent() const
    {
        return m_percent;
    }

    // "48\n" -> 48. Leading whitespace is skipped; anything that is not a
    // whole number in 0..100 is invalid (-1).
    static int parsePercent(const std::string& content);

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
    std::string m_path;
    int m_percent = -1;
    unsigned m_sinceReadMs = 0;
    bool m_hasRead = false;
};

} // namespace miyoofin

#endif // MIYOOFIN_BATTERY_MONITOR_HPP
