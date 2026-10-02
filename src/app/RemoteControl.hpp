#ifndef MIYOOFIN_REMOTE_CONTROL_HPP
#define MIYOOFIN_REMOTE_CONTROL_HPP

#include "../input/Action.hpp"

#include <string>
#include <vector>

namespace miyoofin {

/// Developer remote control: lets a person (or an AI over SSH) press logical buttons and
/// quit the app by writing lines to /tmp/miyoofin-app-cmd. It is OFF unless the file
/// `.remote-control` exists in the app directory, so a normal install never reads it.
///
/// Lines: `key <Name>` (Up Down Left Right Confirm Back Search ActionsMenu PrevTab NextTab
/// PrevPage NextPage Settings Menu) or `quit`. Unknown lines are ignored.
bool remoteControlEnabled();

/// Pure parser (testable): appends the actions for `text`; `quit` becomes Action::Exit.
void parseRemoteCommands(const std::string& text, std::vector<Action>& out);

/// Reads and consumes the command file when remote control is enabled.
void pollRemoteControl(std::vector<Action>& out);

} // namespace miyoofin

#endif
