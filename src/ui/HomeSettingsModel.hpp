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
    CheckForUpdates,
    MusicMode,
    TestConnection,
    CrashReport,
    ClockFormat,
    TimeZone,
    AudioLanguage,
    SubtitleLanguage
};
struct HomeSettingsAddressRow
{
    std::string section;
    std::string value;
    HomeSettingsRowAction action;
};

constexpr int homeSettingsBaseRowCount()
{
    return 18; // fixed: offline, 2 addresses, test connection, 11 info/action rows, change server,
               // log out
}
std::vector<HomeSettingsAddressRow> homeSettingsAddressRows(const Session& session);

/// One card of the Settings tab, in display order. `key` names the content of read-only cards
/// (action None): "sync", "storage", "route", "about", "account".
struct HomeSettingsRow
{
    HomeSettingsRowAction action;
    const char*
        group; // small tag on the card: LIBRARY, PLAYBACK, DISPLAY, CONNECTION, APP, ACCOUNT
    const char* key;
};
const std::vector<HomeSettingsRow>& homeSettingsRows(const Session& session);
int homeSettingsRowCount(const Session& session);
HomeSettingsRowAction homeSettingsRowAction(int row, const Session& session);

enum class HomeSettingsConfirmation
{
    None,
    ChangeServer,
    Logout,
    CheckForUpdates
};

// UI-thread selection/scroll state and the two-press confirmation for the
// Settings tab. Performs no I/O: Home carries out whatever a confirmed press
// means (request flags, update install).
class HomeSettingsState
{
  public:
    static constexpr int kVisibleRows = 6;

    int selected = 0;
    int scroll = 0;
    HomeSettingsConfirmation confirmation = HomeSettingsConfirmation::None;

    // Up (delta < 0) / Down (delta > 0) within [0, rowCount). Changing the
    // selection drops a pending confirmation; scroll is always re-clamped.
    void move(int delta, int rowCount);

    // Re-clamps scroll for the current selection (tab switches).
    void clampScroll(int rowCount);

    // Back: clears a pending confirmation. True when one was pending.
    bool cancelConfirmation();

    // Two-press confirmation. True when this press confirms `requested` (the
    // confirmation is cleared); false when it only arms it.
    bool press(HomeSettingsConfirmation requested);
};

} // namespace miyoofin

#endif // MIYOOFIN_HOME_SETTINGS_MODEL_HPP
