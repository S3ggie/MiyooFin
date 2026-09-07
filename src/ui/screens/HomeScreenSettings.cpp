#include "HomeScreen.hpp"

namespace miyoofin {

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row)
{ return homeSettingsRowAction(row); }

std::vector<HomeScreen::SettingsAddressRow> HomeScreen::settingsAddressRows(const Session &session)
{ return homeSettingsAddressRows(session); }

int HomeScreen::settingsRowCount(const Session &session)
{ return homeSettingsRowCount(session); }

HomeScreen::SettingsRowAction HomeScreen::settingsRowAction(int row, const Session &session)
{ return homeSettingsRowAction(row, session); }

} // namespace miyoofin
