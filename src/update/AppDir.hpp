#ifndef MIYOOFIN_UPDATE_APPDIR_HPP
#define MIYOOFIN_UPDATE_APPDIR_HPP

#include <string>

namespace miyoofin {

/// Resolve the directory containing the running executable.
/// Uses readlink("/proc/self/exe") + dirname; falls back to getcwd().
/// Returns false on failure.
bool appDir(std::string &out);

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_APPDIR_HPP
