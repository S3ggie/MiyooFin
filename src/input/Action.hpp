#ifndef MIYOOFIN_ACTION_HPP
#define MIYOOFIN_ACTION_HPP

namespace miyoofin {

/// Logical action enum.
/// The mapping from physical buttons to logical actions is tentative
/// and will be refined once the real Miyoo key mapping is confirmed
/// via the input diagnostics screen.
enum class Action {
    None,

    // Navigation
    Up,
    Down,
    Left,
    Right,

    // Confirm / select
    Confirm,        // A

    // Back / cancel
    Back,           // B

    // Context actions
    Search,         // X
    ActionsMenu,    // Y

    // Shoulder buttons
    PrevTab,        // L1
    NextTab,        // R1
    PrevPage,       // L2
    NextPage,       // R2

    // System
    Settings,       // START
    Menu,           // SELECT  (context / menu)
    Exit,           // MENU / POWER

    // Special for diagnostics
    Raw
};

/// Human-readable name for debug/diagnostics display
inline const char *actionName(Action a) {
    switch (a) {
        case Action::None:         return "None";
        case Action::Up:           return "Up";
        case Action::Down:         return "Down";
        case Action::Left:         return "Left";
        case Action::Right:        return "Right";
        case Action::Confirm:      return "Confirm (A)";
        case Action::Back:         return "Back (B)";
        case Action::Search:       return "Search (X)";
        case Action::ActionsMenu:  return "Actions (Y)";
        case Action::PrevTab:      return "PrevTab (L1)";
        case Action::NextTab:      return "NextTab (R1)";
        case Action::PrevPage:     return "PrevPage (L2)";
        case Action::NextPage:     return "NextPage (R2)";
        case Action::Settings:     return "Settings (START)";
        case Action::Menu:         return "Menu (SELECT)";
        case Action::Exit:         return "Exit (MENU)";
        case Action::Raw:          return "Raw";
    }
    return "???";
}

} // namespace miyoofin

#endif // MIYOOFIN_ACTION_HPP