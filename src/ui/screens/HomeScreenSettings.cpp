#include "../UiKit.hpp"
#include <cctype>
#include <algorithm>
#include "HomeScreen.hpp"
#include "../BitmapFont.hpp"
#include "../Theme.hpp"
#include "../../download/DownloadSupport.hpp"
#include "miyoofin/version.hpp"
#include <ctime>

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

void HomeScreen::drawSettingsTab(SDL_Surface* fb)
{
    const auto syncStatus = m_libraryCoordinator ? m_libraryCoordinator->status()
                                                 : library::LibraryCoordinator::Status{};
    struct SettingRow
    {
        std::string section;
        std::string value;
    };
    std::vector<SettingRow> rows = {{"Offline Mode", m_session.manualOfflineMode ? "ON" : "OFF"}};
    for (const SettingsAddressRow& row : settingsAddressRows(m_session))
        rows.push_back({row.section, row.value});
    rows.insert(rows.end(),
                {{"Last API Route", lastApiRouteValue()},
                 {"Account", m_userName.empty() ? "Unknown" : m_userName},
                 {"LIBRARY", "Last Sync: " + compactSyncAge(syncStatus.lastSuccessfulMs)},
                 {"DOWNLOADS", "Local " + formatBytes(m_downloadsState.snapshot.localBytes) +
                                   " | Free " + formatBytes(m_downloadsState.snapshot.freeBytes)},
                 {"DIAGNOSTICS", "UI Stall Logger Enabled"},
                 {"MIYOOFIN MUSIC", "Press A to enter MiyooFin Music"}});
    // UPDATES row — directly above ABOUT.
    {
        std::string updateValue = updateStatusText(m_updateSnapshot);
        if (m_updateSnapshot.stage == UpdateStage::Available &&
            m_settingsState.confirmation == SettingsConfirmation::CheckForUpdates) {
            updateValue = "Press A again to install v" + m_updateSnapshot.availableVersion;
        }
        rows.push_back({"UPDATES", updateValue});
    }
    rows.insert(rows.end(), {{"ABOUT", std::string(APP_NAME) + " " + VERSION_STR},
                             {"SERVER", "Sign in to a different server"},
                             {"ACCOUNT", "Log Out"}});
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
        if (selected)
            ui::focusRing(fb, d::kMargin, y, W, ROW_H);
        ui::roundFill(fb, d::kMargin, y, W, ROW_H, d::kRadius, selected ? d::kRaised : d::kPanel);
        ui::roundOutline(fb, d::kMargin, y, W, ROW_H, d::kRadius,
                         selected ? d::kAccent : d::kBorder);
        std::string label = rows[index].section;
        for (char& c : label)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        ui::text(fb, d::kMargin + 14, y + 10, label, selected ? d::kAccentHi : d::kTextMuted);
        const std::string& raw = rows[index].value;
        d::Rgb color = selected ? d::kText : d::kTextSecondary;
        if (label == "OFFLINE MODE")
            color = raw == "ON" ? d::kWarning : d::kSuccess;
        else if (label == "ACCOUNT" && raw == "Log Out")
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
