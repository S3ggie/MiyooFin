#ifndef MIYOOFIN_PLAYBACK_REQUEST_HPP
#define MIYOOFIN_PLAYBACK_REQUEST_HPP

#include <string>

namespace miyoofin {

/// Write a playback-request.txt file so that launch.sh can orchestrate
/// bridged FFplay playback after MiyooFin exits cleanly.
///
/// The request file contains only non-secret information:
///   item_id=<Jellyfin item ID>
///   item_type=movie|episode
///
/// The access token is NOT stored here; launch.sh reads it from session.txt.
class PlaybackRequest {
public:
    /// Write a playback request to the default path ("playback-request.txt").
    /// @param itemId    Jellyfin item ID (must be non-empty).
    /// @param itemType  "movie" or "episode".
    /// @param error     Set on failure with a human-readable message.
    /// @return true on success.
    static bool write(const std::string &itemId,
                      const std::string &itemType,
                      std::string &error);

    /// Check whether a playback request file currently exists.
    static bool exists();

    /// Remove the playback request file (consumed or invalid).
    /// Returns true if removed or did not exist.
    static bool remove();

    /// Default file path.
    static const char *defaultPath();

    // For testing: write/load from an explicit path.
    static bool writeTo(const std::string &path,
                        const std::string &itemId,
                        const std::string &itemType,
                        std::string &error);

    /// Check whether a playback request file exists at an explicit path.
    static bool existsAt(const std::string &path);

    /// Remove the file at an explicit path.
    static bool removeAt(const std::string &path);
};

} // namespace miyoofin

#endif // MIYOOFIN_PLAYBACK_REQUEST_HPP
