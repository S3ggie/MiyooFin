#ifndef MIYOOFIN_UPDATE_INSTALLER_HPP
#define MIYOOFIN_UPDATE_INSTALLER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace miyoofin {

// -------------------------------------------------------------------
// ELF / ARM helpers — exposed for unit testing
// -------------------------------------------------------------------

/// Return true if the first 5 bytes match the ELF magic: 0x7f 'E' 'L' 'F'.
bool isElfMagic(const unsigned char* buf, std::size_t len);

/// Return true if the ELF header indicates ARM (e_machine == 0x28).
/// Expects the raw ELF header bytes; returns false if too short.
bool isElfArm(const unsigned char* buf, std::size_t len);

// -------------------------------------------------------------------
// Tar listing parser — exposed for unit testing
// -------------------------------------------------------------------

/// Parse one `tar -tv` listing line.  `filename` receives the stripped path
/// (before any " -> " / " link to " target).  Returns false if unparseable.
/// Exposed for unit testing.
bool parseTarListingLine(const std::string& line, std::string& filename, bool& isSymlink,
                         bool& isHardlink);

// -------------------------------------------------------------------
// installUpdate — the OTA install engine
// -------------------------------------------------------------------

/// Install a downloaded OTA update archive into *appDir*.
///
/// All work happens under *appDir*; user state (session.txt,
/// downloads/, cache/) is never touched.
///
/// Steps:
///   1. List the archive contents (tar -tzf) and detect symlinks (tar -tvzf).
///   2. Build a safe install plan via buildInstallPlan().
///   3. Extract ONLY the planned MiyooFin/ paths.
///   4. Sanity-check the extracted miyoofin binary (ELF + ARM).
///   5. Back up current copies of planned files.
///   6. Install each file atomically (write .tmp -> fsync -> rename),
///      replacing miyoofin LAST.
///   7. Write update-applied.json atomically.
///
/// On failure: best-effort rollback from backup, then return false.
///
/// @param appDir          Application root (e.g. /mnt/mmc/Apps/MiyooFin).
/// @param tarGzPath       Path to the downloaded .tar.gz archive.
/// @param targetVersion   The version being installed (for backup dir name).
/// @param error           Human-readable error on failure.
/// @param cancelled       Cooperative cancellation flag (checked between steps).
/// @param progress        Progress callback (0-100 percent).
/// @return true on success.
bool installUpdate(const std::string& appDir, const std::string& tarGzPath,
                   const std::string& targetVersion, std::string& error,
                   const std::atomic<bool>* cancelled = nullptr,
                   const std::function<void(int percent)>& progress = {});

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_INSTALLER_HPP
