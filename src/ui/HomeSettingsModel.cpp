#include "HomeSettingsModel.hpp"
#include "../net/ServerAddress.hpp"
#include <algorithm>

namespace miyoofin {

std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session& session)
{
    const bool lanOnly = session.localServerUrl.empty() && isObviousLanServerUrl(session.serverUrl);
    std::vector<HomeSettingsAddressRow> rows;
    rows.push_back({lanOnly ? "LAN Server" : "Public Server",
                    session.serverUrl.empty() ? "Not connected" : session.serverUrl,
                    HomeSettingsRowAction::ChangeServer});
    if (lanOnly)
        rows.push_back({"Public Address",
                        session.publicServerUrl.empty() ? "Not Set" : session.publicServerUrl,
                        HomeSettingsRowAction::PublicAddress});
    else
        rows.push_back({"Local Address",
                        session.localServerUrl.empty() ? "Not Set" : session.localServerUrl,
                        HomeSettingsRowAction::LocalAddress});
    return rows;
}

int homeSettingsRowCount(const Session& session)
{
    return 1 + (int)homeSettingsAddressRows(session).size() + 9;
}

HomeSettingsRowAction homeSettingsRowAction(int row, const Session& session)
{
    if (row == 0)
        return HomeSettingsRowAction::OfflineMode;
    const std::vector<HomeSettingsAddressRow> addresses = homeSettingsAddressRows(session);
    if (row >= 1 && row <= static_cast<int>(addresses.size()))
        return addresses[row - 1].action;
    const int count = homeSettingsRowCount(session);
    if (row == count - 3)
        return HomeSettingsRowAction::CheckForUpdates;
    if (row == count - 5)
        return HomeSettingsRowAction::DownloadAudio;
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
