#ifndef MIYOOFIN_HOME_SETTINGS_MODEL_HPP
#define MIYOOFIN_HOME_SETTINGS_MODEL_HPP

#include "../net/Session.hpp"
#include <string>
#include <vector>

namespace miyoofin {

enum class HomeSettingsRowAction { None, OfflineMode, ChangeServer, LocalAddress, PublicAddress, Logout };
struct HomeSettingsAddressRow { std::string section; std::string value; HomeSettingsRowAction action; };

constexpr int homeSettingsBaseRowCount() { return 9; }
std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session &session);
int homeSettingsRowCount(const Session &session);
HomeSettingsRowAction homeSettingsRowAction(int row);
HomeSettingsRowAction homeSettingsRowAction(int row, const Session &session);

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SETTINGS_MODEL_HPP
