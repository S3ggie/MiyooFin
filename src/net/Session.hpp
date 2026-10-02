#ifndef MIYOOFIN_SESSION_HPP
#define MIYOOFIN_SESSION_HPP

#include <string>

namespace miyoofin {

/// Persistent authentication session.
/// Saved to disk as key=value lines.
/// The password is never saved — only the access token.
struct Session
{
    std::string serverUrl;
    std::string serverId;
    /// Optional verified local route.  serverUrl remains the canonical identity.
    std::string localServerUrl;
    /// Optional verified public fallback when the canonical identity is LAN-only.
    std::string publicServerUrl;
    std::string accessToken;
    std::string userId;
    std::string userName;
    std::string deviceId;
    /// True once `localServerUrl` / `publicServerUrl` hold the two addresses exactly as the user
    /// set them (either may be empty). Older session files and in-memory sessions only have
    /// `serverUrl`, from which the two addresses are derived (see routes()).
    bool routesExplicit = false;
    /// User-selected downloaded-only library presentation.  This is not a
    /// connectivity state and must not affect requests or playback reporting.
    bool manualOfflineMode = false;

    /// The two ways to reach the server. `lan` is tried first when set; `pub` is the fallback
    /// (or the only route). `serverUrl` is only the identity that caches and downloads are
    /// filed under, so changing an address never orphans them.
    struct Routes
    {
        std::string lan, pub;
    };
    Routes routes() const;
    /// Stores the addresses as given (either may be empty, not both).
    void setRoutes(const std::string& lan, const std::string& pub);
    /// Converts an older-shaped session to explicit addresses (a no-op otherwise).
    void makeRoutesExplicit();

    /// True if the session contains enough data to attempt token validation.
    bool valid() const
    {
        return !accessToken.empty() && !userId.empty() && !serverUrl.empty();
    }

    /// Reset all fields to empty.
    void clear()
    {
        serverUrl.clear();
        serverId.clear();
        localServerUrl.clear();
        publicServerUrl.clear();
        accessToken.clear();
        userId.clear();
        userName.clear();
        deviceId.clear();
        manualOfflineMode = false;
        routesExplicit = false;
    }

    /// Save to the default path ("session.txt") in the current directory.
    /// Returns true on success.
    bool save() const;

    /// Load from the default path.
    static Session load();

    /// Delete the session file. Returns true on success.
    static bool remove();

    /// Save to an explicit path (for testing).
    bool saveTo(const std::string& path) const;

    /// Load from an explicit path (for testing).
    static Session loadFrom(const std::string& path);
};

} // namespace miyoofin

#endif // MIYOOFIN_SESSION_HPP
