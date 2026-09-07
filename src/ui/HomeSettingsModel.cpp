#include "HomeSettingsModel.hpp"
#include "../net/ServerAddress.hpp"

namespace miyoofin {

HomeSettingsRowAction homeSettingsRowAction(int row)
{
    switch (row) {
    case 0: return HomeSettingsRowAction::OfflineMode;
    case 1: return HomeSettingsRowAction::ChangeServer;
    case 2: return HomeSettingsRowAction::LocalAddress;
    case 8: return HomeSettingsRowAction::Logout;
    default: return HomeSettingsRowAction::None;
    }
}

std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session &session)
{
    const bool lanOnly=session.localServerUrl.empty() && isObviousLanServerUrl(session.serverUrl);
    std::vector<HomeSettingsAddressRow> rows;
    rows.push_back({lanOnly ? "LAN Server" : "Public Server",
                    session.serverUrl.empty() ? "Not connected" : session.serverUrl,
                    HomeSettingsRowAction::ChangeServer});
    if (lanOnly)
        rows.push_back({"Public Address", session.publicServerUrl.empty() ? "Not Set" : session.publicServerUrl,
                        HomeSettingsRowAction::PublicAddress});
    else
        rows.push_back({"Local Address", session.localServerUrl.empty() ? "Not Set" : session.localServerUrl,
                        HomeSettingsRowAction::LocalAddress});
    return rows;
}

int homeSettingsRowCount(const Session &session)
{
    return 1+(int)homeSettingsAddressRows(session).size()+7;
}

HomeSettingsRowAction homeSettingsRowAction(int row, const Session &session)
{
    if (row==0) return HomeSettingsRowAction::OfflineMode;
    const std::vector<HomeSettingsAddressRow> addresses=homeSettingsAddressRows(session);
    if (row>=1 && row<=static_cast<int>(addresses.size())) return addresses[row-1].action;
    return row==homeSettingsRowCount(session)-1 ? HomeSettingsRowAction::Logout : HomeSettingsRowAction::None;
}

} // namespace miyoofin
