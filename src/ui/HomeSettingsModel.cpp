#include "HomeSettingsModel.hpp"
#include "../net/ServerAddress.hpp"
#include <algorithm>

namespace miyoofin {

std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session& session)
{
    // Two plainly named addresses, each optional on its own (at least one must stay).
    const Session::Routes routes = session.routes();
    return {{"Home network address", routes.lan.empty() ? "Not set" : routes.lan,
             HomeSettingsRowAction::LocalAddress},
            {"Internet address", routes.pub.empty() ? "Not set" : routes.pub,
             HomeSettingsRowAction::PublicAddress}};
}

const std::vector<HomeSettingsRow>& homeSettingsRows(const Session&)
{
    using A = HomeSettingsRowAction;
    // Grouped by what the user is doing: library, playback, display, connection, the app, the
    // account (the risky rows last).
    static const std::vector<HomeSettingsRow> rows = {
        {A::OfflineMode, "LIBRARY", ""},       {A::None, "LIBRARY", "sync"},
        {A::None, "LIBRARY", "storage"},       {A::AudioLanguage, "PLAYBACK", ""},
        {A::SubtitleLanguage, "PLAYBACK", ""}, {A::ClockFormat, "DISPLAY", ""},
        {A::TimeZone, "DISPLAY", ""},          {A::LocalAddress, "CONNECTION", ""},
        {A::PublicAddress, "CONNECTION", ""},  {A::TestConnection, "CONNECTION", ""},
        {A::None, "CONNECTION", "route"},      {A::MusicMode, "APP", ""},
        {A::CheckForUpdates, "APP", ""},       {A::None, "APP", "about"},
        {A::CrashReport, "APP", ""},           {A::None, "ACCOUNT", "account"},
        {A::ChangeServer, "ACCOUNT", ""},      {A::Logout, "ACCOUNT", ""},
    };
    return rows;
}

int homeSettingsRowCount(const Session& session)
{
    return static_cast<int>(homeSettingsRows(session).size());
}

HomeSettingsRowAction homeSettingsRowAction(int row, const Session& session)
{
    const auto& rows = homeSettingsRows(session);
    return row >= 0 && row < static_cast<int>(rows.size()) ? rows[row].action
                                                           : HomeSettingsRowAction::None;
}

void HomeSettingsState::move(int delta, int rowCount)
{
    const int previous = selected;
    if (delta < 0 && selected > 0)
        --selected;
    else if (delta > 0 && selected < rowCount - 1)
        ++selected;
    if (selected != previous)
        confirmation = HomeSettingsConfirmation::None;
    clampScroll(rowCount);
}

void HomeSettingsState::clampScroll(int rowCount)
{
    scroll = std::max(0, std::min(selected, rowCount - kVisibleRows));
}

bool HomeSettingsState::cancelConfirmation()
{
    if (confirmation == HomeSettingsConfirmation::None)
        return false;
    confirmation = HomeSettingsConfirmation::None;
    return true;
}

bool HomeSettingsState::press(HomeSettingsConfirmation requested)
{
    if (confirmation == requested) {
        confirmation = HomeSettingsConfirmation::None;
        return true;
    }
    confirmation = requested;
    return false;
}

} // namespace miyoofin
