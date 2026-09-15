#include "UpdateInstallPlan.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace miyoofin {

// -------------------------------------------------------------------
// Path helpers
// -------------------------------------------------------------------

/// Check whether *path* has the prefix "MiyooFin/" (case-sensitive).
static bool hasMiyooFinPrefix(const std::string &path)
{
    const std::string prefix = "MiyooFin/";
    if (path.size() < prefix.size()) return false;
    return path.compare(0, prefix.size(), prefix) == 0;
}

/// Check whether any path component in *rel* is ".." or empty.
static bool hasUnsafeComponent(const std::string &rel)
{
    size_t start = 0;
    while (start < rel.size()) {
        size_t end = rel.find('/', start);
        if (end == std::string::npos) end = rel.size();
        size_t len = end - start;
        if (len == 0) return true;           // empty component (double slash)
        if (len == 2 && rel[start] == '.' && rel[start + 1] == '.')
            return true;                     // ".." component
        start = end + 1;
    }
    return false;
}

/// Check whether *rel* matches the blacklist patterns.
/// BLACKLIST: session.txt, cache/**, downloads/**, telemetry-logs/**,
///            *.log, *.bmp, *.bak, update-*
static bool isBlacklisted(const std::string &rel)
{
    // Exact matches
    if (rel == "session.txt") return true;

    // Prefix matches (directory blacklists)
    static const char *prefixes[] = {
        "cache/", "downloads/", "telemetry-logs/"
    };
    for (auto *p : prefixes) {
        if (rel.compare(0, std::strlen(p), p) == 0) return true;
    }

    // Glob-style suffix matches
    auto hasSuffix = [](const std::string &s, const char *suffix) -> bool {
        size_t sl = std::strlen(suffix);
        if (s.size() < sl) return false;
        return s.compare(s.size() - sl, sl, suffix) == 0;
    };
    if (hasSuffix(rel, ".log") || hasSuffix(rel, ".bmp") ||
        hasSuffix(rel, ".bak"))
        return true;

    // Prefix glob: update-*
    if (rel.compare(0, 7, "update-") == 0) return true;

    return false;
}

// -------------------------------------------------------------------
bool isWhitelistedAppPath(const std::string &rel)
{
    // Blacklisted paths are never whitelisted.
    if (isBlacklisted(rel)) return false;

    // Exact matches (top-level files)
    static const char *exact[] = {
        "miyoofin",
        "miyoofin-https-bridge",
        "miyoofin-perf-reporter",
        "cacert.pem",
        "launch.sh",
        "playback_runner.sh",
        "config.json",
        "icon.png",
        "LICENSE",
        "THIRD_PARTY_NOTICES.md",
    };
    for (auto *e : exact) {
        if (rel == e) return true;
    }

    // assets/placeholder.png
    if (rel == "assets/placeholder.png") return true;

    // lib/*.so and lib/*.so.*
    if (rel.compare(0, 4, "lib/") == 0) {
        // Find ".so" in the filename part and check it's at end or followed by '.'
        size_t soPos = rel.rfind(".so");
        if (soPos != std::string::npos && soPos >= 4) {
            size_t afterSo = soPos + 3;
            if (afterSo == rel.size()) return true;            // ends with .so
            if (afterSo < rel.size() && rel[afterSo] == '.') return true; // .so.*
        }
    }

    return false;
}

// -------------------------------------------------------------------
bool isSafeTarEntry(const std::string &entry,
                    std::string &normalizedOut)
{
    // Reject absolute paths
    if (!entry.empty() && entry[0] == '/') return false;

    // Must have MiyooFin/ prefix
    if (!hasMiyooFinPrefix(entry)) return false;

    // Strip MiyooFin/ prefix
    normalizedOut = entry.substr(9); // strlen("MiyooFin/") == 9

    // Reject empty after stripping
    if (normalizedOut.empty()) return false;

    // Reject any ".." component
    if (hasUnsafeComponent(normalizedOut)) return false;

    return true;
}

// -------------------------------------------------------------------
// Install plan ordering categories.
// -------------------------------------------------------------------

/// Return a sort priority for an entry.  Lower values come first.
/// miyoofin itself always returns INT_MAX so it ends up last.
static int planPriority(const std::string &rel)
{
    if (rel == "miyoofin") return 1000; // always last

    // Libraries
    if (rel.compare(0, 4, "lib/") == 0) return 100;

    // Legal / documentation
    if (rel == "LICENSE" || rel == "THIRD_PARTY_NOTICES.md") return 200;

    // Scripts
    if (rel == "launch.sh" || rel == "playback_runner.sh") return 300;

    // Bridge / reporter binaries
    if (rel == "miyoofin-https-bridge" || rel == "miyoofin-perf-reporter")
        return 400;

    // Config / assets
    if (rel == "config.json" || rel == "icon.png" ||
        rel == "assets/placeholder.png" || rel == "cacert.pem")
        return 500;

    return 600; // everything else
}

// -------------------------------------------------------------------
std::vector<std::string> buildInstallPlan(
    const std::vector<std::string> &tarListing,
    std::string &error)
{
    std::vector<std::string> plan;
    bool foundBinary = false;

    for (auto &entry : tarListing) {
        std::string normalized;
        if (!isSafeTarEntry(entry, normalized)) {
            error = "unsafe entry: " + entry;
            return {};
        }
        if (!isWhitelistedAppPath(normalized)) {
            error = "entry not whitelisted: " + normalized;
            return {};
        }
        if (normalized == "miyoofin") foundBinary = true;
        plan.push_back(normalized);
    }

    if (!foundBinary) {
        error = "required binary MiyooFin/miyoofin missing from archive";
        return {};
    }

    // Sort by priority; stable sort preserves original order within
    // equal-priority groups.  miyoofin (priority 1000) ends up last.
    std::stable_sort(plan.begin(), plan.end(),
        [](const std::string &a, const std::string &b) {
            return planPriority(a) < planPriority(b);
        });

    return plan;
}

} // namespace miyoofin
