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

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_HPP
