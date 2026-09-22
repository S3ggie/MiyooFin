#ifndef MIYOOFIN_HOME_SETTINGS_MODEL_HPP
#define MIYOOFIN_HOME_SETTINGS_MODEL_HPP

#include "../net/Session.hpp"
#include <string>
#include <vector>

namespace miyoofin {

enum class HomeSettingsRowAction
{
    None,
    OfflineMode,
    ChangeServer,
    LocalAddress,
    PublicAddress,
    Logout,
    CheckForUpdates
};
struct HomeSettingsAddressRow
{
    std::string section;
    std::string value;
    HomeSettingsRowAction action;
};

constexpr int homeSettingsBaseRowCount()
{
    return 10;
}
std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session& session);
int homeSettingsRowCount(const Session& session);
HomeSettingsRowAction homeSettingsRowAction(int row, const Session& session);

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SETTINGS_MODEL_HPP
