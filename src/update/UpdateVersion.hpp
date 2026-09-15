#ifndef MIYOOFIN_UPDATE_VERSION_HPP
#define MIYOOFIN_UPDATE_VERSION_HPP

#include <string>

namespace miyoofin {

/// Semantic version triple with optional prerelease identifier.
struct SemVer {
    int major = 0;
    int minor = 0;
    int patch = 0;
    std::string prerelease; // empty for a release version
};

/// Parse a semver string such as "v0.2.0", "=1.0.0-beta.1", "0.2.0+build.3".
/// Strips a leading `v` or `=`, requires X.Y.Z numeric core, optional
/// `-prerelease` and ignored `+build` metadata.  Returns false on malformed
/// input.
bool parseSemVer(const std::string &in, SemVer &out);

/// Compare two parsed SemVer values.
///   - Numeric triple compared first.
///   - A release (no prerelease) sorts AFTER a prerelease of the same triple.
///   - Prerelease strings compared per semver-ish rules: split on `.`, numeric
///     identifiers compared numerically (and numerics sort less than
///     alphanumerics), otherwise lexicographic.
/// Returns -1, 0, or +1.
int compareSemVer(const SemVer &a, const SemVer &b);

/// Convenience: returns true only when both versions parse and candidate > current.
/// Malformed input -> false.
bool isNewerThan(const std::string &candidate, const std::string &current);

} // namespace miyoofin

#endif // MIYOOFIN_UPDATE_VERSION_HPP
