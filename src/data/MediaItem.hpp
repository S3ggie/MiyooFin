#ifndef MIYOOFIN_MEDIA_ITEM_HPP
#define MIYOOFIN_MEDIA_ITEM_HPP

#include <string>
#include <vector>
#include <map>
#include <SDL2/SDL.h>

namespace miyoofin {

/// Represents a single movie, show, or episode.
struct MediaItem {
    std::string id;
    std::string title;
    std::string overview;
    int         year = 0;
    float       rating = 0.0f;       // Community rating (0.0–10.0 from Jellyfin)
    std::string genre;               // First genre (for compact display)
    std::string type;                // "movie", "show", or "episode"
    std::string etag;                // Jellyfin Etag, when supplied

    // Checkpoint B4: additional fields from Jellyfin
    std::vector<std::string> genres = {}; // All genres from server
    bool        played = false;      // User has fully watched
    float       progress = 0.0f;     // 0.0–1.0 playback progress
    long long   playbackPositionTicks = 0; // Exact Jellyfin UserData resume position
    std::map<std::string, std::string> imageTags = {}; // e.g. {"Primary":"tagId"}
    int         indexNumber = 0;     // Jellyfin IndexNumber (season/episode number)

    // Checkpoint B5e2a: episode metadata fields
    int         parentIndexNumber = 0; // Jellyfin ParentIndexNumber (season number for episodes)
    long long   runTimeTicks = 0;      // Jellyfin RunTimeTicks (10,000,000 ticks/sec)
    std::string seriesName;            // Jellyfin SeriesName
    std::string seriesId;              // Jellyfin SeriesId
    std::string seasonId;              // Jellyfin SeasonId

    // Placeholder artwork tint colour (for the coloured rectangle)
    Uint8 artR = 128, artG = 128, artB = 128;
};

/// A horizontal row of media items (one section on the home screen).
struct MediaRow {
    std::string            label;
    std::vector<MediaItem> items;
};

/// A top-level tab with its rows.
struct TabData {
    std::string           name;
    std::vector<MediaRow> rows;
};

/// Convert Jellyfin RunTimeTicks (10,000,000 ticks/sec) to whole minutes.
/// 0 ticks -> 0; positive ticks -> rounded to nearest whole minute.
inline int ticksToMinutes(long long ticks)
{
    if (ticks <= 0) return 0;
    long long seconds = (ticks + 5000000LL) / 10000000LL; // round to nearest second
    return static_cast<int>((seconds + 30) / 60);          // round to nearest minute
}

/// Compute playback progress percentage (0–100) from MediaItem fields.
/// Prefers calculating from playbackPositionTicks / runTimeTicks when
/// runtime is valid; falls back to the server-provided progress field.
inline int playbackPercent(const MediaItem &item)
{
    if (item.runTimeTicks > 0 && item.playbackPositionTicks > 0) {
        long long pos = item.playbackPositionTicks;
        if (pos > item.runTimeTicks) pos = item.runTimeTicks;
        double pct = static_cast<double>(pos) / static_cast<double>(item.runTimeTicks) * 100.0;
        if (pct < 0.0) pct = 0.0;
        if (pct > 100.0) pct = 100.0;
        return static_cast<int>(pct + 0.5);
    }
    // Fall back to server-provided progress (0.0–1.0 from PlayedPercentage).
    double pct = item.progress * 100.0;
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return static_cast<int>(pct + 0.5);
}

/// Format compact playback time strings from tick values.
/// Returns e.g. "29m watched • 18m remaining" or "1h 24m watched • 32m remaining".
/// Returns empty string if runtime is unknown or invalid.
inline std::string formatPlaybackTime(long long positionTicks, long long runTimeTicks)
{
    if (runTimeTicks <= 0) return {};
    if (positionTicks < 0) positionTicks = 0;
    if (positionTicks > runTimeTicks) positionTicks = runTimeTicks;

    int watchedMins = ticksToMinutes(positionTicks);
    int totalMins   = ticksToMinutes(runTimeTicks);
    int remainingMins = totalMins - watchedMins;
    if (remainingMins < 0) remainingMins = 0;

    auto compactMin = [](int mins) -> std::string {
        if (mins >= 60) {
            int h = mins / 60;
            int m = mins % 60;
            return std::to_string(h) + "h " + std::to_string(m) + "m";
        }
        return std::to_string(mins) + "m";
    };

    return compactMin(watchedMins) + " watched | " + compactMin(remainingMins) + " remaining";
}

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_HPP
