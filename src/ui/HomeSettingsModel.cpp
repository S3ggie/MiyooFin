#include "HomeSettingsModel.hpp"
#include "../net/ServerAddress.hpp"

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
    return 1 + (int)homeSettingsAddressRows(session).size() + 8;
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
    return row == count - 1 ? HomeSettingsRowAction::Logout : HomeSettingsRowAction::None;
}

} // namespace miyoofin
