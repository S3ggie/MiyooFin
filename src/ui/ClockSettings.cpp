#include "ClockSettings.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace miyoofin {

const std::vector<ClockZone>& clockZones()
{
    static const std::vector<ClockZone> zones = {
        {"eastern", "Eastern (US, Canada)", "EST5EDT,M3.2.0,M11.1.0",
         "America/New_York America/Detroit America/Toronto America/Montreal America/Indiana/Indianapolis "
         "America/Kentucky/Louisville America/Nassau"},
        {"central", "Central (US, Canada)", "CST6CDT,M3.2.0,M11.1.0",
         "America/Chicago America/Winnipeg America/Menominee America/Indiana/Knox America/Matamoros"},
        {"mountain", "Mountain (US, Canada)", "MST7MDT,M3.2.0,M11.1.0",
         "America/Denver America/Edmonton America/Boise America/Yellowknife"},
        {"arizona", "Arizona (no DST)", "MST7", "America/Phoenix"},
        {"pacific", "Pacific (US, Canada)", "PST8PDT,M3.2.0,M11.1.0",
         "America/Los_Angeles America/Vancouver America/Tijuana"},
        {"alaska", "Alaska", "AKST9AKDT,M3.2.0,M11.1.0", "America/Anchorage America/Juneau"},
        {"hawaii", "Hawaii", "HST10", "Pacific/Honolulu"},
        {"atlantic", "Atlantic (Canada)", "AST4ADT,M3.2.0,M11.1.0", "America/Halifax Atlantic/Bermuda"},
        {"mexico", "Mexico City", "CST6", "America/Mexico_City America/Monterrey America/Guatemala"},
        {"brazil", "Brasilia", "<-03>3", "America/Sao_Paulo America/Argentina/Buenos_Aires"},
        {"utc", "UTC", "UTC0", "UTC Etc/UTC"},
        {"london", "London, Dublin", "GMT0BST,M3.5.0/1,M10.5.0",
         "Europe/London Europe/Dublin"},
        {"lisbon", "Lisbon", "WET0WEST,M3.5.0/1,M10.5.0", "Europe/Lisbon Atlantic/Canary"},
        {"cet", "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3",
         "Europe/Paris Europe/Berlin Europe/Madrid Europe/Rome Europe/Amsterdam Europe/Brussels "
         "Europe/Vienna Europe/Zurich Europe/Stockholm Europe/Oslo Europe/Copenhagen Europe/Prague "
         "Europe/Warsaw Europe/Budapest Europe/Belgrade Europe/Zagreb Europe/Luxembourg Europe/Malta"},
        {"eet", "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4",
         "Europe/Athens Europe/Helsinki Europe/Kiev Europe/Kyiv Europe/Bucharest Europe/Sofia "
         "Europe/Riga Europe/Tallinn Europe/Vilnius"},
        {"moscow", "Moscow, Istanbul", "<+03>-3",
         "Europe/Moscow Europe/Minsk Europe/Istanbul Asia/Riyadh Asia/Baghdad"},
        {"dubai", "Dubai", "<+04>-4", "Asia/Dubai"},
        {"india", "India", "IST-5:30", "Asia/Kolkata Asia/Calcutta"},
        {"bangkok", "Bangkok, Jakarta", "<+07>-7", "Asia/Bangkok Asia/Jakarta Asia/Ho_Chi_Minh"},
        {"china", "China, Singapore, Perth", "CST-8",
         "Asia/Shanghai Asia/Singapore Asia/Hong_Kong Asia/Taipei Asia/Manila Australia/Perth"},
        {"japan", "Japan, Korea", "JST-9", "Asia/Tokyo Asia/Seoul"},
        {"sydney", "Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3",
         "Australia/Sydney Australia/Melbourne Australia/Canberra Australia/Hobart"},
        {"brisbane", "Brisbane", "AEST-10", "Australia/Brisbane"},
        {"auckland", "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3", "Pacific/Auckland"},
    };
    return zones;
}

const ClockZone* clockZoneForIana(const std::string& ianaName)
{
    for (const ClockZone& z : clockZones()) {
        std::istringstream names(z.iana);
        std::string name;
        while (names >> name)
            if (name == ianaName)
                return &z;
    }
    return nullptr;
}

const ClockZone* clockZoneForKey(const std::string& key)
{
    for (const ClockZone& z : clockZones())
        if (key == z.key)
            return &z;
    return nullptr;
}

ClockSettings::ClockSettings()
{
    if (const char* tz = std::getenv("TZ")) {
        m_hadTz = true;
        m_originalTz = tz;
    }
}

ClockSettings& ClockSettings::instance()
{
    static ClockSettings settings;
    return settings;
}

void ClockSettings::load(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return;
    char line[160];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (s.compare(0, 7, "hour24=") == 0)
            m_hour24 = s.substr(7) != "0";
        else if (s.compare(0, 5, "zone=") == 0)
            m_zoneMode = s.substr(5);
        else if (s.compare(0, 9, "detected=") == 0)
            m_detectedIana = s.substr(9);
    }
    std::fclose(f);
    if (m_zoneMode != "auto" && m_zoneMode != "device" && !clockZoneForKey(m_zoneMode))
        m_zoneMode = "auto";
}

bool ClockSettings::save(const std::string& path) const
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    std::fprintf(f, "hour24=%d\nzone=%s\ndetected=%s\n", m_hour24 ? 1 : 0, m_zoneMode.c_str(),
                 m_detectedIana.c_str());
    const bool ok = std::fclose(f) == 0;
    return ok && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::string ClockSettings::zoneSummary() const
{
    if (m_zoneMode == "device")
        return "Device setting";
    if (m_zoneMode == "auto") {
        const ClockZone* z = clockZoneForIana(m_detectedIana);
        if (z)
            return std::string("Automatic: ") + z->label;
        return m_detectedIana.empty() ? "Automatic (not found yet)" : "Automatic: " + m_detectedIana;
    }
    const ClockZone* z = clockZoneForKey(m_zoneMode);
    return z ? z->label : "Device setting";
}

void ClockSettings::applyZone() const
{
    const ClockZone* z = nullptr;
    if (m_zoneMode == "auto")
        z = clockZoneForIana(m_detectedIana);
    else if (m_zoneMode != "device")
        z = clockZoneForKey(m_zoneMode);
    if (z) {
        ::setenv("TZ", z->posix, 1);
    } else if (m_zoneMode == "device") { // back to what the system provided
        if (m_hadTz)
            ::setenv("TZ", m_originalTz.c_str(), 1);
        else
            ::unsetenv("TZ");
    } else {
        return; // automatic but nothing found yet: keep what the system gave us
    }
    ::tzset();
}

std::string ClockSettings::format(const std::tm& local) const
{
    char buf[16];
    if (m_hour24) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", local.tm_hour, local.tm_min);
    } else {
        const int h = local.tm_hour % 12 == 0 ? 12 : local.tm_hour % 12;
        std::snprintf(buf, sizeof(buf), "%d:%02d %s", h, local.tm_min,
                      local.tm_hour < 12 ? "AM" : "PM");
    }
    return buf;
}

std::string ClockSettings::formatNow() const
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return format(local);
}

} // namespace miyoofin
