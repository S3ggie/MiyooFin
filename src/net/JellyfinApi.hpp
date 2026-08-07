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

/// Result of a successful AuthenticateByName call.
struct AuthResult {
    std::string accessToken;
    std::string userId;
    std::string userName;
    std::string serverId;
};

/// Categorised error for authentication attempts.
enum class AuthError {
    None,               ///< Success
    Network,            ///< Could not reach server
    InvalidCredentials, ///< Wrong username/password (401)
    Unauthorized,       ///< Account disabled / not authorised (403)
    BadRequest,         ///< Bad request / missing fields (400)
    ServerError,        ///< HTTP 5xx or other error
    ParseError          ///< Could not parse authentication response
};

/// Minimal Jellyfin API helper.
/// Supports the public /System/Info endpoint and
/// /Users/AuthenticateByName for username/password login.
class JellyfinApi {
public:
    /// Call GET /System/Info/Public on the given server base URL.
    static bool getSystemInfo(const std::string &baseUrl,
                              ServerInfo &info,
                              std::string &error);

    /// Authenticate with username/password via POST /Users/AuthenticateByName.
    /// @param baseUrl      Server base URL (e.g. "http://192.168.1.50:8096").
    /// @param username     Jellyfin username (JSON-escaped internally).
    /// @param password     Password (never logged or saved).
    /// @param deviceId     Persistent device identifier.
    /// @param result       Output: access token, user info (on success).
    /// @param errCode      Output: categorised error code.
    /// @param error        Output: human-readable error message.
    /// @return true on successful authentication.
    static bool authenticateByName(const std::string &baseUrl,
                                   const std::string &username,
                                   const std::string &password,
                                   const std::string &deviceId,
                                   AuthResult &result,
                                   AuthError &errCode,
                                   std::string &error);

    /// Validate an existing access token via GET /Users/{userId}.
    /// @return true if the token is valid (HTTP 200).
    static bool validateToken(const std::string &baseUrl,
                              const std::string &accessToken,
                              const std::string &userId,
                              const std::string &deviceId,
                              std::string &error);

    /// Normalise a user-entered URL.
    static std::string normaliseUrl(const std::string &input);

private:
    /// Extract a top-level JSON string value by key.
    static std::string extractString(const std::string &json, const std::string &key);

    /// Find a JSON string value nested under a parent key.
    /// e.g. extractNestedString(json, "User", "Id").
    static std::string extractNestedString(const std::string &json,
                                           const std::string &parent,
                                           const std::string &child);

    /// Escape a string for embedding in a JSON value.
    static std::string jsonEscape(const std::string &s);

    /// Classify an HTTP response from AuthenticateByName.
    static AuthError classifyAuthError(long httpStatus, const std::string &body,
                                       std::string &message);
};

} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_API_HPP