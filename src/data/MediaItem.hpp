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
    std::map<std::string, std::string> imageTags = {}; // e.g. {"Primary":"tagId"}
    int         indexNumber = 0;     // Jellyfin IndexNumber (season/episode number)

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

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_HPP