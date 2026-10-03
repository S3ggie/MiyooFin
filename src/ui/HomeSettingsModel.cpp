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

int homeSettingsRowCount(const Session& session)
{
    return 1 + (int)homeSettingsAddressRows(session).size() + 15;
}

HomeSettingsRowAction homeSettingsRowAction(int row, const Session& session)
{
    if (row == 0)
        return HomeSettingsRowAction::OfflineMode;
    const std::vector<HomeSettingsAddressRow> addresses = homeSettingsAddressRows(session);
    if (row >= 1 && row <= static_cast<int>(addresses.size()))
        return addresses[row - 1].action;
    if (row == static_cast<int>(addresses.size()) + 1)
        return HomeSettingsRowAction::TestConnection;
    const int count = homeSettingsRowCount(session);
    if (row == count - 10)
        return HomeSettingsRowAction::CrashReport;
    if (row == count - 9)
        return HomeSettingsRowAction::ClockFormat;
    if (row == count - 8)
        return HomeSettingsRowAction::TimeZone;
    if (row == count - 7)
        return HomeSettingsRowAction::AudioLanguage;
    if (row == count - 6)
        return HomeSettingsRowAction::SubtitleLanguage;
    if (row == count - 5)
        return HomeSettingsRowAction::MusicMode;
    if (row == count - 4)
        return HomeSettingsRowAction::CheckForUpdates;
    if (row == count - 2)
        return HomeSettingsRowAction::ChangeServer;
    return row == count - 1 ? HomeSettingsRowAction::Logout : HomeSettingsRowAction::None;
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
