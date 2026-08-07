#ifndef MIYOOFIN_JELLYFIN_API_HPP
#define MIYOOFIN_JELLYFIN_API_HPP

#include <string>

namespace miyoofin {

/// Server info returned by the /System/Info endpoint.
struct ServerInfo {
    std::string serverName;
    std::string version;
    std::string operatingSystem;
};

/// Minimal Jellyfin API helper.
/// Currently only supports the public /System/Info endpoint.
/// No authentication is required for this endpoint.
class JellyfinApi {
public:
    /// Call GET /System/Info/Public on the given server base URL.
    /// This is a public endpoint that does not require authentication.
    /// @param info     Output: populated on success
    /// @param error    Output: human-readable error on failure
    /// @return true on success
    static bool getSystemInfo(const std::string &baseUrl,
                              ServerInfo &info,
                              std::string &error);

    /// Normalise a user-entered URL:
    ///   - Strip trailing whitespace
    ///   - Prepend "http://" if no scheme is present
    ///   - Strip trailing '/'
    static std::string normaliseUrl(const std::string &input);
};

} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_API_HPP