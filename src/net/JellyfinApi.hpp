#ifndef MIYOOFIN_JELLYFIN_API_HPP
#define MIYOOFIN_JELLYFIN_API_HPP

#include <string>
#include <vector>
#include <map>
#include "../data/MediaItem.hpp"

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

/// A user's media library / collection view.
struct LibraryView {
    std::string id;
    std::string name;
    std::string collectionType; // "movies", "tvshows", "music", etc.
};

/// Minimal Jellyfin API helper.
/// Supports the public /System/Info endpoint,
/// /Users/AuthenticateByName for username/password login,
/// and authenticated library/data fetching.
class JellyfinApi {
public:
    /// Call GET /System/Info/Public on the given server base URL.
    static bool getSystemInfo(const std::string &baseUrl,
                              ServerInfo &info,
                              std::string &error);

    /// Authenticate with username/password via POST /Users/AuthenticateByName.
    static bool authenticateByName(const std::string &baseUrl,
                                   const std::string &username,
                                   const std::string &password,
                                   const std::string &deviceId,
                                   AuthResult &result,
                                   AuthError &errCode,
                                   std::string &error);

    /// Validate an existing access token via GET /Users/{userId}.
    static bool validateToken(const std::string &baseUrl,
                              const std::string &accessToken,
                              const std::string &userId,
                              const std::string &deviceId,
                              std::string &error);

    /// Normalise a user-entered URL.
    static std::string normaliseUrl(const std::string &input);

    // ---- Checkpoint B4: library / media fetching ---------------------------

    /// Fetch the authenticated user's library views (collections).
    static bool getViews(const std::string &baseUrl,
                         const std::string &accessToken,
                         const std::string &userId,
                         const std::string &deviceId,
                         std::vector<LibraryView> &views,
                         std::string &error);

    /// Fetch items from a specific library.
    /// @param includeItemTypes  e.g. "Movie" or "Series"
    static bool getLibraryItems(const std::string &baseUrl,
                                const std::string &accessToken,
                                const std::string &userId,
                                const std::string &deviceId,
                                const std::string &parentId,
                                const std::string &includeItemTypes,
                                int limit,
                                std::vector<MediaItem> &items,
                                std::string &error);

    /// Fetch "continue watching" / resume items.
    static bool getResumeItems(const std::string &baseUrl,
                               const std::string &accessToken,
                               const std::string &userId,
                               const std::string &deviceId,
                               int limit,
                               std::vector<MediaItem> &items,
                               std::string &error);

    /// Fetch "recently added" items across all libraries.
    static bool getLatestItems(const std::string &baseUrl,
                               const std::string &accessToken,
                               const std::string &userId,
                               const std::string &deviceId,
                               int limit,
                               std::vector<MediaItem> &items,
                               std::string &error);

    /// Fetch seasons for a given series.
    static bool getSeasons(const std::string &baseUrl,
                           const std::string &accessToken,
                           const std::string &userId,
                           const std::string &deviceId,
                           const std::string &seriesId,
                           std::vector<MediaItem> &seasons,
                           std::string &error);

    /// Fetch episodes for a given season of a series.
    static bool getEpisodes(const std::string &baseUrl,
                            const std::string &accessToken,
                            const std::string &userId,
                            const std::string &deviceId,
                            const std::string &seriesId,
                            const std::string &seasonId,
                            std::vector<MediaItem> &episodes,
                            std::string &error);

    // ---- URL helpers (public for testing) ----------------------------------

    static std::string buildLatestUrl(const std::string &baseUrl,
                                      const std::string &userId,
                                      int limit);

    // ---- JSON parsing helpers (public for testing) -------------------------

    static std::string jsonStringField(const std::string &obj,
                                       const std::string &key);
    static int jsonIntField(const std::string &obj, const std::string &key);
    static float jsonFloatField(const std::string &obj, const std::string &key);
    static bool jsonBoolField(const std::string &obj, const std::string &key);
    static std::vector<std::string> jsonExtractArray(const std::string &json,
                                                     const std::string &key);
    static MediaItem jsonToMediaItem(const std::string &obj);

    /// Build TabData from fetched library data.
    static std::vector<TabData> buildTabs(
        const std::vector<LibraryView> &views,
        const std::vector<MediaItem> &continueWatching,
        const std::vector<MediaItem> &recentlyAdded,
        const std::vector<std::pair<std::string, std::vector<MediaItem>>> &moviesByView,
        const std::vector<std::pair<std::string, std::vector<MediaItem>>> &showsByView);

    // ---- Auth header helper (public for HomeScreen artwork loading) --------

    /// Build standard X-Emby auth headers.
    static std::vector<std::string> buildAuthHeaders(
        const std::string &accessToken,
        const std::string &deviceId);

private:
    static std::string extractString(const std::string &json,
                                     const std::string &key);
    static std::string extractNestedString(const std::string &json,
                                           const std::string &parent,
                                           const std::string &child);
    static std::string jsonEscape(const std::string &s);
    static AuthError classifyAuthError(long httpStatus, const std::string &body,
                                       std::string &message);

    /// Extract a raw JSON value (string, number, bool, null, object, array).
    static std::string jsonRawValue(const std::string &json,
                                    const std::string &key);

    /// Split comma-separated elements from an array body string.
    static std::vector<std::string> splitJsonArrayContent(
        const std::string &content);
};

} // namespace miyoofin

#endif // MIYOOFIN_JELLYFIN_API_HPP