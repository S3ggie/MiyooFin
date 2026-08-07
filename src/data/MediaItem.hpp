#ifndef MIYOOFIN_MEDIA_ITEM_HPP
#define MIYOOFIN_MEDIA_ITEM_HPP

#include <string>
#include <SDL2/SDL.h>

namespace miyoofin {

/// Represents a single movie or TV show for the mock UI.
struct MediaItem {
    std::string id;
    std::string title;
    std::string overview;
    int         year;
    float       rating;       // 0.0 – 5.0
    std::string genre;
    std::string type;         // "movie" or "show"

    // Placeholder artwork tint colour (for the coloured rectangle)
    Uint8 artR, artG, artB;
};

} // namespace miyoofin

#endif // MIYOOFIN_MEDIA_ITEM_HPP