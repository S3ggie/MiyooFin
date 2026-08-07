#ifndef MIYOOFIN_SESSION_HPP
#define MIYOOFIN_SESSION_HPP

#include <string>

namespace miyoofin {

/// Persistent authentication session.
/// Saved to disk as key=value lines.
/// The password is never saved — only the access token.
struct Session {
    std::string serverUrl;
    std::string accessToken;
    std::string userId;
    std::string userName;
    std::string deviceId;

    /// True if the session contains enough data to attempt token validation.
    bool valid() const {
        return !accessToken.empty() && !userId.empty() && !serverUrl.empty();
    }

    /// Reset all fields to empty.
    void clear() {
        serverUrl.clear();
        accessToken.clear();
        userId.clear();
        userName.clear();
        deviceId.clear();
    }

    /// Save to the default path ("session.txt") in the current directory.
    /// Returns true on success.
    bool save() const;

    /// Load from the default path.
    static Session load();

    /// Delete the session file. Returns true on success.
    static bool remove();

    /// Save to an explicit path (for testing).
    bool saveTo(const std::string &path) const;

    /// Load from an explicit path (for testing).
    static Session loadFrom(const std::string &path);
};

} // namespace miyoofin

#endif // MIYOOFIN_SESSION_HPP