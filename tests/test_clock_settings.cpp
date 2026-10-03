#include "test_support.hpp"

#include "../src/ui/ClockSettings.hpp"
#include <cstdio>
#include <cstdlib>

using namespace miyoofin;

int main()
{
    CHECK(clockZoneForIana("America/Chicago") &&
          std::string(clockZoneForIana("America/Chicago")->key) == "central");
    CHECK(clockZoneForIana("Europe/Rome") &&
          std::string(clockZoneForIana("Europe/Rome")->key) == "cet");
    CHECK(clockZoneForIana("Mars/Olympus") == nullptr);

    std::tm t{};
    t.tm_hour = 21;
    t.tm_min = 5;
    ClockSettings s;
    CHECK_EQ(s.format(t), "21:05");
    s.setHour24(false);
    CHECK_EQ(s.format(t), "9:05p");
    t.tm_hour = 0;
    CHECK_EQ(s.format(t), "12:05a");
    t.tm_hour = 12;
    CHECK_EQ(s.format(t), "12:05p");

    // Central rule: 03:00 UTC on 2026-01-15 is 21:00 the evening before; in July it is 22:00.
    ClockSettings c;
    c.setZoneMode("central");
    c.applyZone();
    std::time_t winter = 1768446000; // 2026-01-15 03:00:00 UTC
    std::time_t summer = 1784084400; // 2026-07-15 03:00:00 UTC
    std::tm local{};
    localtime_r(&winter, &local);
    CHECK(local.tm_hour == 21);
    localtime_r(&summer, &local);
    CHECK(local.tm_hour == 22);

    // Round trip through the file; an unknown stored zone falls back to automatic.
    const std::string path = "clock-settings-test.txt";
    ClockSettings a;
    a.setHour24(false);
    a.setZoneMode("auto");
    a.setDetected("America/Chicago");
    CHECK(a.save(path));
    ClockSettings b;
    b.load(path);
    CHECK(!b.hour24() && b.zoneMode() == "auto" && b.detectedIana() == "America/Chicago");
    CHECK_EQ(b.zoneSummary(), "Automatic: Central (US, Canada)");
    std::remove(path.c_str());
    return miyoofin_test::finish("clock_settings");
}
