#ifndef MIYOOFIN_CLOCK_SETTINGS_HPP
#define MIYOOFIN_CLOCK_SETTINGS_HPP

#include <ctime>
#include <string>
#include <vector>

namespace miyoofin {

/// One choosable time zone: a short label, a POSIX TZ rule (the device has no zoneinfo files)
/// and the IANA names that map onto it (for detection from the network).
struct ClockZone
{
    const char* key;   // stored in clock-settings.txt
    const char* label; // shown in the picker
    const char* posix; // TZ value, DST rules included
    const char* iana;  // space-separated IANA names, e.g. "America/Chicago America/Winnipeg"
};

const std::vector<ClockZone>& clockZones();
/// Zone whose IANA list contains `ianaName` (nullptr when unknown).
const ClockZone* clockZoneForIana(const std::string& ianaName);
const ClockZone* clockZoneForKey(const std::string& key);

/// The clock the header shows: 12 or 24 hour, and which time zone. Persisted in
/// `clock-settings.txt`. "auto" follows the zone found from the network (cached); "device"
/// leaves the zone the system provides alone. UI thread only (the detection result is handed
/// over with setDetected()).
class ClockSettings
{
  public:
    static ClockSettings& instance();
    ClockSettings();

    void load(const std::string& path);
    bool save(const std::string& path) const;

    bool hour24() const
    {
        return m_hour24;
    }
    void setHour24(bool on)
    {
        m_hour24 = on;
    }
    /// "auto", "device" or a ClockZone key.
    const std::string& zoneMode() const
    {
        return m_zoneMode;
    }
    void setZoneMode(const std::string& mode)
    {
        m_zoneMode = mode;
    }
    const std::string& detectedIana() const
    {
        return m_detectedIana;
    }
    void setDetected(const std::string& iana)
    {
        m_detectedIana = iana;
    }
    /// Short text for Settings rows: "Automatic (Central)", "Eastern", "Device setting".
    std::string zoneSummary() const;
    /// Sets TZ for this process from the current choice (no-op for "device").
    void applyZone() const;
    /// "21:05" or "9:05 PM".
    std::string format(const std::tm& local) const;
    std::string formatNow() const;

    /// Asks the app to look the zone up from the network (it does so on a worker).
    void requestDetect()
    {
        m_detectRequested = true;
    }
    bool takeDetectRequest()
    {
        const bool r = m_detectRequested;
        m_detectRequested = false;
        return r;
    }

  private:
    bool m_detectRequested = false;
    bool m_hadTz = false;
    std::string m_originalTz; // what the system gave us, restored for "Device setting"
    bool m_hour24 = true;
    std::string m_zoneMode = "auto";
    std::string m_detectedIana;
};

} // namespace miyoofin

#endif
