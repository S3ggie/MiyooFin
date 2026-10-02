#ifndef MIYOOFIN_APP_MODE_HPP
#define MIYOOFIN_APP_MODE_HPP

#include <string>

namespace miyoofin {

/// MiyooFin is one app with two modes: the video client and MiyooFin Music. The mode last
/// used is remembered across launches.
enum class AppMode
{
    Video,
    Music
};

/// Reads the saved mode; anything missing or unreadable means Video.
AppMode loadAppMode(const std::string& path = "app-mode.txt");
bool saveAppMode(AppMode mode, const std::string& path = "app-mode.txt");

} // namespace miyoofin

#endif
