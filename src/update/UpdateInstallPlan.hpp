#ifndef MIYOOFIN_UPDATE_INSTALL_PLAN_HPP
#define MIYOOFIN_UPDATE_INSTALL_PLAN_HPP

#include <string>
#include <vector>

namespace miyoofin {

/// Check whether a relative path (after stripping the MiyooFin/ prefix)
/// is on the update whitelist.
bool isWhitelistedAppPath(const std::string &relativeOut);

/// Validate a single tar entry.  Returns true if the entry is safe to
/// extract, setting *normalizedOut* to the path relative to the app root
/// (without the leading MiyooFin/ prefix).
///
/// Rejects:
///   - Absolute paths
///   - Paths containing a `..` component
///   - Entries not under the MiyooFin/ prefix
///
/// Note: symlink/hardlink detection is handled separately in the
/// collapsed tar -tvzf pass (tarListAndDetectLinks), before this
/// function is called.
bool isSafeTarEntry(const std::string &entry,
                    std::string &normalizedOut);

/// Build a safe install plan from a `tar -tzf`-style listing (one path
/// per line).  Each entry must pass isSafeTarEntry and be whitelisted.
///
/// Returns the ordered list of paths to extract (relative to app root).
/// The required binary `miyoofin` is guaranteed to be LAST.
/// Sets *error* and returns empty if any entry is unsafe or if the
/// required `miyoofin` binary is missing from the archive.
std::vector<std::string> buildInstallPlan(
    const std::vector<std::string> &tarListing,
    std::string &error);

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_INSTALL_PLAN_HPP
