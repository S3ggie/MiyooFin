#include "../UiKit.hpp"
#include <cctype>
#include <algorithm>
#include "HomeScreen.hpp"
#include "../BitmapFont.hpp"
#include "../ClockSettings.hpp"
#include "../Theme.hpp"
#include "../../download/DownloadSupport.hpp"
#include "miyoofin/version.hpp"
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {

static std::int64_t wallClockMs()
{
    return static_cast<std::int64_t>(std::time(nullptr)) * 1000;
}

static std::string compactSyncAge(std::int64_t timestamp)
{
    if (timestamp <= 0)
        return "Never";
    const std::int64_t elapsed = std::max<std::int64_t>(0, wallClockMs() - timestamp);
    if (elapsed < 60LL * 1000)
        return "Now";
    if (elapsed < 60LL * 60 * 1000)
        return std::to_string(elapsed / (60LL * 1000)) + "m ago";
    if (elapsed < 24LL * 60 * 60 * 1000)
        return std::to_string(elapsed / (60LL * 60 * 1000)) + "h ago";
    return std::to_string(elapsed / (24LL * 60 * 60 * 1000)) + "d ago";
}

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row)
{
    // Legacy 1-arg wrapper: uses a public session (2 address rows).
    // Matches the 2-arg overload for non-LAN sessions.
    Session pub;
    pub.serverUrl = "https://jellyfin.example.com";
    pub.localServerUrl = "http://192.168.1.5:8096";
    return homeSettingsRowAction(row, pub);
}

std::vector<HomeScreen::SettingsAddressRow> HomeScreen::settingsAddressRows(const Session& session)
{
    return homeSettingsAddressRows(session);
}

int HomeScreen::settingsRowCount(const Session& session)
{
    return homeSettingsRowCount(session);
}

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row, const Session& session)
{
    return homeSettingsRowAction(row, session);
}

namespace {
bool hasCrashLog()
{
    struct stat st;
    return ::stat("crash.log", &st) == 0 && st.st_size > 0;
}
constexpr const char* kPlayerPrefsPath = "player-prefs.txt";
struct Language
{
    const char* code;
    const char* name;
};
// ISO 639-2 codes as the player sees them in the file's track tags.
constexpr Language kLanguages[] = {{"eng", "English"},    {"jpn", "Japanese"}, {"spa", "Spanish"},
                                   {"fre", "French"},     {"ger", "German"},   {"ita", "Italian"},
                                   {"por", "Portuguese"}, {"kor", "Korean"},   {"chi", "Chinese"},
                                   {"rus", "Russian"}};
std::string languageName(const std::string& code)
{
    for (const Language& l : kLanguages)
        if (code == l.code)
            return l.name;
    return code;
}
} // namespace

std::string HomeScreen::languageValue(bool audio)
{
    if (!m_playerPrefsLoaded) {
        m_playerPrefs = PlayerPrefs::load(kPlayerPrefsPath);
        m_playerPrefsLoaded = true;
    }
    if (audio)
        return m_playerPrefs.audioLang.empty() ? "Automatic"
                                               : languageName(m_playerPrefs.audioLang);
    if (m_playerPrefs.subOff)
        return "Off";
    return m_playerPrefs.subLang.empty() ? "Automatic" : languageName(m_playerPrefs.subLang);
}

void HomeScreen::startConnectionTest()
{
    const Session::Routes routes = m_session.routes();
    const std::string serverId = m_session.serverId;
    const bool started =
        m_connectionTest.start([this, routes, serverId](const CancelToken& cancel) {
            auto check = [&](const std::string& url) -> std::string {
                if (url.empty())
                    return "not set";
                ServerInfo info;
                std::string error;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = JellyfinApi::getSystemInfo(url, info, error, cancel.get(), 20);
                const long ms =
                    static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - t0)
                                          .count());
                if (!ok)
                    return error.empty() ? "failed" : "failed (" + error.substr(0, 40) + ")";
                if (!serverId.empty() && !JellyfinApi::serverIdsMatch(serverId, info.serverId))
                    return "different server";
                return "OK " + std::to_string(ms) + " ms";
            };
            const std::string home = check(routes.lan);
            const std::string internet = check(routes.pub);
            m_connectionTestPending = "Home: " + home + "  |  Internet: " + internet;
        });
    if (started)
        m_connectionTestResult = "Testing...";
}

void HomeScreen::openZoneMenu()
{
    m_zoneMenu = true;
    std::vector<std::string> items = {"Automatic (from network)", "Device setting"};
    for (const ClockZone& z : clockZones())
        items.push_back(z.label);
    m_languageMenu.open("Time zone", items);
}

void HomeScreen::applyZoneChoice(int index)
{
    ClockSettings& clock = ClockSettings::instance();
    if (index == 0) {
        clock.setZoneMode("auto");
        clock.requestDetect();
    } else if (index == 1) {
        clock.setZoneMode("device");
    } else if (index - 2 < static_cast<int>(clockZones().size())) {
        clock.setZoneMode(clockZones()[index - 2].key);
    }
    clock.applyZone();
    clock.save("clock-settings.txt");
}

void HomeScreen::openLanguageMenu(bool audio)
{
    m_zoneMenu = false;
    languageValue(audio); // load once
    m_languageIsAudio = audio;
    std::vector<std::string> items = {"Automatic"};
    if (!audio)
        items.push_back("Off");
    for (const Language& l : kLanguages)
        items.push_back(l.name);
    m_languageMenu.open(audio ? "Default audio language" : "Default subtitles", items);
}

void HomeScreen::applyLanguageChoice(int index)
{
    const int offset = m_languageIsAudio ? 1 : 2;
    PlayerPrefs prefs = PlayerPrefs::load(kPlayerPrefsPath); // keep what the player wrote
    std::string code;
    bool off = false;
    if (index >= offset)
        code = kLanguages[index - offset].code;
    else if (!m_languageIsAudio && index == 1)
        off = true;
    if (m_languageIsAudio) {
        prefs.audioLang = code;
    } else {
        prefs.subLang = code;
        prefs.subOff = off;
    }
    prefs.save(kPlayerPrefsPath);
    m_playerPrefs = prefs;
}

void HomeScreen::drawSettingsTab(SDL_Surface* fb)
{
    const auto syncStatus = m_libraryCoordinator ? m_libraryCoordinator->status()
                                                 : library::LibraryCoordinator::Status{};
    struct SettingRow
    {
        std::string section;
        std::string value;
        std::string group;
        SettingsRowAction action;
    };
    std::vector<SettingRow> rows;
    const std::vector<SettingsAddressRow> addresses = settingsAddressRows(m_session);
    for (const HomeSettingsRow& spec : homeSettingsRows(m_session)) {
        SettingRow row{"", "", spec.group, spec.action};
        const std::string key = spec.key;
        switch (spec.action) {
        case SettingsRowAction::OfflineMode:
            row.section = "Offline Mode";
            row.value = m_session.manualOfflineMode ? "ON" : "OFF";
            break;
        case SettingsRowAction::LocalAddress:
            row.section = addresses[0].section;
            row.value = addresses[0].value;
            break;
        case SettingsRowAction::PublicAddress:
            row.section = addresses[1].section;
            row.value = addresses[1].value;
            break;
        case SettingsRowAction::TestConnection:
            row.section = "Test connection";
            row.value = m_connectionTestResult.empty() ? "Press A to check both addresses"
                                                       : m_connectionTestResult;
            break;
        case SettingsRowAction::CrashReport:
            row.section = "Diagnostics";
            row.value = hasCrashLog() ? "Crash report available - press A" : "No crashes recorded";
            break;
        case SettingsRowAction::ClockFormat:
            row.section = "Clock format";
            row.value = ClockSettings::instance().hour24() ? "24-hour" : "12-hour";
            break;
        case SettingsRowAction::TimeZone:
            row.section = "Time zone";
            row.value = ClockSettings::instance().zoneSummary();
            break;
        case SettingsRowAction::AudioLanguage:
            row.section = "Default audio language";
            row.value = languageValue(true);
            break;
        case SettingsRowAction::SubtitleLanguage:
            row.section = "Default subtitles";
            row.value = languageValue(false);
            break;
        case SettingsRowAction::MusicMode:
            row.section = "MIYOOFIN MUSIC";
            row.value = "Press A to enter MiyooFin Music";
            break;
        case SettingsRowAction::CheckForUpdates:
            row.section = "UPDATES";
            row.value = updateStatusText(m_updateSnapshot);
            if (m_updateSnapshot.stage == UpdateStage::Available &&
                m_settingsState.confirmation == SettingsConfirmation::CheckForUpdates)
                row.value = "Press A again to install v" + m_updateSnapshot.availableVersion;
            break;
        case SettingsRowAction::ChangeServer:
            row.section = "Change server";
            row.value = "Sign in to a different server";
            break;
        case SettingsRowAction::Logout:
            row.section = "Sign out";
            row.value = "Log Out";
            break;
        default:
            if (key == "sync") {
                row.section = "Last sync";
                row.value = compactSyncAge(syncStatus.lastSuccessfulMs);
            } else if (key == "storage") {
                row.section = "Downloads";
                row.value = "Local " + formatBytes(m_downloadsState.snapshot.localBytes) +
                            " | Free " + formatBytes(m_downloadsState.snapshot.freeBytes);
            } else if (key == "route") {
                row.section = "Last API Route";
                row.value = lastApiRouteValue();
            } else if (key == "about") {
                row.section = "ABOUT";
                row.value = std::string(APP_NAME) + " " + VERSION_STR;
            } else {
                row.section = "Signed in as";
                row.value = m_userName.empty() ? "Unknown" : m_userName;
            }
            break;
        }
        rows.push_back(std::move(row));
    }
    namespace d = design;
    constexpr int ROW_H = 58, PITCH = 66, TOP = d::kHeaderH + 10,
                  W = d::kScreenW - 2 * d::kMargin - 8;
    const int total = static_cast<int>(rows.size());
    for (int visible = 0; visible < HomeSettingsState::kVisibleRows; ++visible) {
        const int index = m_settingsState.scroll + visible;
        if (index >= total)
            break;
        const int y = TOP + visible * PITCH;
        const bool selected = index == m_settingsState.selected;
        const bool musicRow = rows[index].section == "MIYOOFIN MUSIC";
        const ui::BrandTone& tone = ui::kMusicTone;
        if (selected && musicRow)
            ui::focusRingTone(fb, d::kMargin, y, W, ROW_H, tone.dim, tone.glow, tone.main,
                              tone.soft, tone.edge);
        else if (selected)
            ui::focusRing(fb, d::kMargin, y, W, ROW_H);
        ui::roundFill(fb, d::kMargin, y, W, ROW_H, d::kRadius,
                      selected ? (musicRow ? tone.raised : d::kRaised) : d::kPanel);
        ui::roundOutline(fb, d::kMargin, y, W, ROW_H, d::kRadius,
                         selected ? (musicRow ? tone.edge : d::kAccent) : d::kBorder);
        if (musicRow) { // "Miyoo" white, "Fin" blue, "Music" purple
            ui::text(fb, d::kMargin + W - 14 - ui::textWidth(rows[index].group), y + 10,
                     rows[index].group, d::kTextMuted);
            ui::brandWord(fb, d::kMargin + 14, y + 10, d::kText, ui::kVideoTone.main, " MUSIC",
                          tone.main);
            ui::textClamped(fb, d::kMargin + 14, y + 30, W - 28, rows[index].value,
                            selected ? d::kText : d::kTextSecondary);
            continue;
        }
        std::string label = rows[index].section;
        for (char& c : label)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ui::text(fb, d::kMargin + 14, y + 10, label, selected ? d::kAccentHi : d::kTextMuted);
        // Which group the card belongs to, small and quiet at the right edge.
        ui::text(fb, d::kMargin + W - 14 - ui::textWidth(rows[index].group), y + 10,
                 rows[index].group, d::kTextMuted);
        const std::string& raw = rows[index].value;
        d::Rgb color = selected ? d::kText : d::kTextSecondary;
        if (label == "OFFLINE MODE")
            color = raw == "ON" ? d::kWarning : d::kSuccess;
        else if (rows[index].action == SettingsRowAction::Logout)
            color = d::kDanger;
        else if (raw == "Not set")
            color = d::kTextMuted;
        ui::textClamped(fb, d::kMargin + 14, y + 30, W - 28, raw, color);
    }
    // Scroll position along the right edge.
    if (total > HomeSettingsState::kVisibleRows) {
        const int trackH = HomeSettingsState::kVisibleRows * PITCH - 8;
        const int thumbH = std::max(16, trackH * HomeSettingsState::kVisibleRows / total);
        const int maxScroll = total - HomeSettingsState::kVisibleRows;
        const int thumbY =
            TOP + (trackH - thumbH) * m_settingsState.scroll / std::max(1, maxScroll);
        ui::fill(fb, d::kScreenW - 10, TOP, 3, trackH, d::kDivider);
        ui::roundFill(fb, d::kScreenW - 10, thumbY, 3, thumbH, 1, d::kAccentDim);
    }
}

} // namespace miyoofin
