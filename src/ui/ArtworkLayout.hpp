#ifndef MIYOOFIN_ARTWORK_LAYOUT_HPP
#define MIYOOFIN_ARTWORK_LAYOUT_HPP

#include "../data/MediaItem.hpp"
#include <vector>

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

/// Row strip height: maximum card height across all media types.
inline constexpr int rowStripHeight()
{
    return 96;
}

/// Compute the virtual X position of card at index ci in a row with
/// mixed-width cards.  Cards start at startX with gap pixels between them.
inline int cardXPosition(const std::vector<MediaItem> &items, int ci,
                          int startX = 4, int gap = 6)
{
    int x = startX;
    for (int i = 0; i < ci && i < (int)items.size(); ++i)
        x += artworkBoxSize(items[i]).w + gap;
    return x;
}

/// Total pixel width from startX through the right edge of the last card
/// (no trailing gap).
inline int totalRowWidth(const std::vector<MediaItem> &items,
                          int startX = 4, int gap = 6)
{
    if (items.empty()) return 0;
    int x = startX;
    for (const auto &item : items)
        x += artworkBoxSize(item).w + gap;
    return x - gap;   // remove trailing gap
}

/// Clamp horizontal pixel scroll so that the selected card is fully visible
/// within the viewport [0 … viewWidth].
///   items       – the row's items
///   activeCard  – index of the selected card
///   curScroll   – current pixel offset
///   viewWidth   – visible width in pixels (e.g. 640)
///   startX      – virtual X of the first card (e.g. 4)
///   gap         – pixels between cards (e.g. 6)
/// Returns the clamped pixel scroll offset (never negative).
inline int clampCardScroll(const std::vector<MediaItem> &items, int activeCard,
                            int curScroll, int viewWidth,
                            int startX = 4, int gap = 6)
{
    if (items.empty() || activeCard < 0 || activeCard >= (int)items.size())
        return 0;

    int scroll = curScroll;
    int cardX  = startX;
    for (int ci = 0; ci < (int)items.size(); ++ci) {
        int w = artworkBoxSize(items[ci]).w;
        if (ci == activeCard) {
            // card left edge must not be left of viewport
            if (cardX - scroll < 0)
                scroll = cardX;
            // card right edge must not be right of viewport
            if (cardX + w - scroll > viewWidth)
                scroll = cardX + w - viewWidth;
        }
        cardX += w + gap;
    }
    if (scroll < 0) scroll = 0;
    return scroll;
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_LAYOUT_HPP
