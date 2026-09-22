#include "UpdateVersion.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>

namespace miyoofin {

// -------------------------------------------------------------------
static std::string trim(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a])))
        a++;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        b--;
    return s.substr(a, b - a);
}

// -------------------------------------------------------------------
bool parseSemVer(const std::string& in, SemVer& out)
{
    std::string s = trim(in);
    if (s.empty())
        return false;

    // Strip leading v or =
    if (s[0] == 'v' || s[0] == '=')
        s = s.substr(1);
    if (s.empty())
        return false;

    // Split off build metadata (after '+')
    std::string buildMeta;
    {
        auto plus = s.find('+');
        if (plus != std::string::npos) {
            buildMeta = s.substr(plus + 1);
            s = s.substr(0, plus);
        }
    }

    // Split off prerelease (after '-')
    std::string pre;
    bool hadDash = false;
    {
        auto dash = s.find('-');
        if (dash != std::string::npos) {
            hadDash = true;
            pre = s.substr(dash + 1);
            s = s.substr(0, dash);
        }
    }

    // Parse X.Y.Z — must be all digits
    int nums[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; i++) {
        if (pos >= s.size() || !std::isdigit(static_cast<unsigned char>(s[pos])))
            return false;
        size_t digitStart = pos;
        unsigned long val = 0;
        while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) {
            val = val * 10 + (s[pos] - '0');
            pos++;
        }
        if (val > static_cast<unsigned long>(INT32_MAX))
            return false;
        // Reject leading zeros (semver: numeric identifiers must not have leading zeros, except for
        // "0" itself)
        if (pos - digitStart > 1 && s[digitStart] == '0')
            return false;
        nums[i] = static_cast<int>(val);
        if (i < 2) {
            if (pos >= s.size() || s[pos] != '.')
                return false;
            pos++;
        }
    }
    if (pos != s.size())
        return false;

    // Validate prerelease characters
    if (hadDash && pre.empty())
        return false; // trailing dash with no prerelease
    if (!pre.empty()) {
        for (char c : pre) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-')
                return false;
        }
        // Must not be empty between dots, must not end with dot
        bool prevDot = true;
        for (char c : pre) {
            if (c == '.') {
                if (prevDot)
                    return false;
                prevDot = true;
            } else {
                prevDot = false;
            }
        }
        if (prevDot)
            return false; // ends with dot

        // Reject leading zeros in purely numeric prerelease identifiers
        // (semver §11).  Mixed alphanumeric ids like "0beta" are allowed.
        size_t i = 0;
        while (i < pre.size()) {
            size_t start = i;
            while (i < pre.size() && pre[i] != '.')
                ++i;
            size_t len = i - start;
            if (len > 1 && pre[start] == '0') {
                bool purelyNumeric =
                    std::all_of(pre.begin() + start, pre.begin() + start + len,
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
                if (purelyNumeric)
                    return false;
            }
            ++i; // skip dot
        }
    }

    out.major = nums[0];
    out.minor = nums[1];
    out.patch = nums[2];
    out.prerelease = pre;
    return true;
}

// -------------------------------------------------------------------
// Split a prerelease string on '.' and compare element-wise.
// Numeric identifiers compared as integers; alphanumerics compared
// lexicographically.  A numeric identifier is always less than a
// non-numeric one.
// -------------------------------------------------------------------
static int comparePrerelease(const std::string& a, const std::string& b)
{
    if (a == b)
        return 0;
    if (a.empty() && !b.empty())
        return 1; // release > prerelease
    if (!a.empty() && b.empty())
        return -1;

    size_t pa = 0, pb = 0;
    while (pa < a.size() || pb < b.size()) {
        // Extract next dot-separated identifier (empty string when exhausted)
        std::string idA, idB;
        while (pa < a.size() && a[pa] != '.')
            idA += a[pa++];
        while (pb < b.size() && b[pb] != '.')
            idB += b[pb++];
        if (pa < a.size())
            pa++; // skip dot
        if (pb < b.size())
            pb++; // skip dot

        // Semver §11: a larger set of pre-release fields has higher
        // precedence when all preceding identifiers are equal.
        if (idA.empty() && !idB.empty())
            return -1;
        if (!idA.empty() && idB.empty())
            return 1;

        bool numA = std::all_of(idA.begin(), idA.end(),
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        bool numB = std::all_of(idB.begin(), idB.end(),
                                [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });

        if (numA && numB) {
            // Numeric comparison — saturate to ULLONG_MAX to keep
            // the comparison total even for identifiers that would
            // overflow unsigned long long.
            constexpr unsigned long long UMAX = std::numeric_limits<unsigned long long>::max();
            auto toULL = [](const std::string& s) -> unsigned long long {
                unsigned long long v = 0;
                for (char c : s) {
                    if (v > UMAX / 10)
                        return UMAX;
                    v = v * 10 + static_cast<unsigned long long>(c - '0');
                }
                return v;
            };
            unsigned long long va = toULL(idA), vb = toULL(idB);
            if (va < vb)
                return -1;
            if (va > vb)
                return 1;
        } else if (numA) {
            return -1; // numeric < alpha
        } else if (numB) {
            return 1;
        } else {
            // Both alpha — lexicographic
            if (idA < idB)
                return -1;
            if (idA > idB)
                return 1;
        }
    }
    return 0;
}

// -------------------------------------------------------------------
int compareSemVer(const SemVer& a, const SemVer& b)
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;

    // Same triple: release (empty prerelease) sorts AFTER prerelease
    if (a.prerelease.empty() && b.prerelease.empty())
        return 0;
    if (a.prerelease.empty())
        return 1; // a is release, b is prerelease
    if (b.prerelease.empty())
        return -1; // a is prerelease, b is release

    return comparePrerelease(a.prerelease, b.prerelease);
}

// -------------------------------------------------------------------
bool isNewerThan(const std::string& candidate, const std::string& current)
{
    SemVer cv, cv2;
    if (!parseSemVer(candidate, cv))
        return false;
    if (!parseSemVer(current, cv2))
        return false;
    return compareSemVer(cv, cv2) > 0;
}

} // namespace miyoofin
