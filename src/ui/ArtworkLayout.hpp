#ifndef MIYOOFIN_ARTWORK_LAYOUT_HPP
#define MIYOOFIN_ARTWORK_LAYOUT_HPP

#include "../data/MediaItem.hpp"

namespace miyoofin {

/// Returned by artworkBoxSize().
struct ArtworkBox {
    int w;
    int h;
};

/// Return the selected-top-artwork box dimensions for a given media item.
///   movie / show / anything-else  →  64 × 96
///   episode                      → 128 × 72
inline ArtworkBox artworkBoxSize(const MediaItem &item)
{
    if (item.type == "episode")
        return {128, 72};
    return {64, 96};
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_LAYOUT_HPP
