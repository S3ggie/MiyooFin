#ifndef MIYOOFIN_PLAYBACK_REQUEST_HPP
#define MIYOOFIN_PLAYBACK_REQUEST_HPP

#include <string>
#include <cstdint>

namespace miyoofin {
struct PlaybackResult { std::string itemId,itemType,sourceMode; std::int64_t positionTicks=0,baseResumeTicks=0; bool serverReported=true; };

/// Write a playback-request.txt file so that launch.sh can orchestrate
/// bridged FFplay playback after MiyooFin exits cleanly.
///
/// The request file contains only non-secret information:
///   item_id=<Jellyfin item ID>
///   item_type=movie|episode
///   resume_ticks=<Jellyfin playback position ticks>
///
/// The access token is NOT stored here; launch.sh reads it from session.txt.
class PlaybackRequest {
public:
    /// Write a playback request to the default path ("playback-request.txt").
    /// @param itemId    Jellyfin item ID (must be non-empty).
    /// @param itemType  "movie" or "episode".
    /// @param resumeTicks Exact Jellyfin resume position; negatives become zero.
    /// @param error     Set on failure with a human-readable message.
    /// @return true on success.
    static bool write(const std::string &itemId,
                      const std::string &itemType,
                      long long resumeTicks,
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
                        long long resumeTicks,
                        std::string &error);

    /// Write an explicit source selection.  Legacy callers remain Jellyfin.
    static bool writeWithSourceTo(const std::string &path, const std::string &itemId,
                                  const std::string &itemType, long long resumeTicks,
                                  const std::string &sourceMode, const std::string &scope,
                                  std::string &error);

    /// Check whether a playback request file exists at an explicit path.
    static bool existsAt(const std::string &path);

    /// Remove the file at an explicit path.
    static bool removeAt(const std::string &path);

    /// Consume the default playback-result.txt for expectedItemId.
    /// The file is removed after the attempt, whether valid or not.
    static bool consumeResult(const std::string &expectedItemId,
                              std::int64_t &positionTicks,
                              std::string &error);

    /// Testable explicit-path form of consumeResult().
    static bool consumeResultFrom(const std::string &path,
                                  const std::string &expectedItemId,
                                  std::int64_t &positionTicks,
                                  std::string &error);
    static bool parseResult(const std::string &content, PlaybackResult &result, std::string &error);
    static bool readResultFrom(const std::string &path, PlaybackResult &result, std::string &error);

    /// Advance the one-update delay used around blocking external playback.
    /// Returns true once, on the first update eligible to consume a result.
    static bool advanceResultConsumption(bool &pending, int &delayUpdates);
};

} // namespace miyoofin

#endif // MIYOOFIN_PLAYBACK_REQUEST_HPP
