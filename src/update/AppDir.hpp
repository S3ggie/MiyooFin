#ifndef MIYOOFIN_UPDATE_APPDIR_HPP
#define MIYOOFIN_UPDATE_APPDIR_HPP

#include <string>

namespace miyoofin {

/// Resolve the directory containing the running executable.
/// Uses readlink("/proc/self/exe") + dirname; falls back to getcwd().
/// Returns false on failure.
bool appDir(std::string& out);

/// Pure helper: compute the SQLite temp directory path from a base directory.
/// Returns baseDir + "/cache/tmp".  The caller is responsible for creating
/// the directory and setting the SQLite temp-directory variable.
inline std::string sqliteTempDirPath(const std::string& baseDir)
{
    return baseDir + "/cache/tmp";
}

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_APPDIR_HPP
