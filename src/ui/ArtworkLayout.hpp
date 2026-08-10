#ifndef MIYOOFIN_ARTWORK_LAYOUT_HPP
#define MIYOOFIN_ARTWORK_LAYOUT_HPP

#include "../data/MediaItem.hpp"
#include "../image/ImageDecoder.hpp"
#include "../cache/ImageCache.hpp"
#include <cstdio>
#include <string>
#include <vector>

namespace miyoofin {

/// Returned by artworkBoxSize().
struct ArtworkBox {
    int w;
    int h;
};

/// The single source of truth for display/cache artwork selection.
struct DisplayArtwork {
    ImageType imageType = ImageType::Primary;
    std::string tag;
    int width = 0;
    int height = 0;
    bool valid() const { return !tag.empty() && width > 0 && height > 0; }
};

inline DisplayArtwork displayArtworkForItem(const MediaItem &item)
{
    const int w = item.type == "episode" ? 128 : 64;
    const int h = item.type == "episode" ? 72 : 96;
    if (item.type == "episode") {
        auto thumb = item.imageTags.find("Thumb");
        if (thumb != item.imageTags.end() && !thumb->second.empty()) return {ImageType::Thumb, thumb->second, w, h};
    }
    auto primary = item.imageTags.find("Primary");
    if (primary != item.imageTags.end() && !primary->second.empty()) return {ImageType::Primary, primary->second, w, h};
    return {};
}

inline const char *imageTypeName(ImageType type) { return type == ImageType::Thumb ? "Thumb" : "Primary"; }

/// Maximum decoded row-artwork images kept in RAM (B5d2a).
static constexpr int ROW_ARTWORK_RAM_LIMIT = 64;
static constexpr int MOVIE_ARTWORK_DECODE_BUDGET = 4;
struct MovieArtworkRange { int first; int lastExclusive; };
inline MovieArtworkRange movieVisibleArtworkRange(int scrollRow, int itemCount)
{
    if (scrollRow < 0) scrollRow = 0;
    int first = scrollRow * 9;
    if (first > itemCount) first = itemCount;
    int last = first + 36;
    if (last > itemCount) last = itemCount;
    return {first, last};
}

// Pure Movies grid helpers.  The grid is deliberately index based so it is
// independent of SDL and straightforward to test.
inline int movieGridRow(int index) { return index < 0 ? 0 : index / 9; }
inline int movieGridColumn(int index) { return index < 0 ? 0 : index % 9; }
inline int moveMovieGrid(int index, int count, int deltaRow, int deltaCol)
{
    if (count <= 0) return 0;
    if (index < 0) index = 0;
    if (index >= count) index = count - 1;
    int row = movieGridRow(index) + deltaRow, col = movieGridColumn(index) + deltaCol;
    if (col < 0 || col >= 9 || row < 0) return index;
    int target = row * 9 + col;
    if (target >= count) {
        if (deltaRow > 0) target = count - 1; // nearest valid item in final row
        else return index;
    }
    return target;
}
inline int clampMovieGridScroll(int selected, int itemCount, int scrollRow)
{
    if (itemCount <= 0) return 0;
    int lastRow = (itemCount - 1) / 9;
    int row = movieGridRow(selected);
    if (row < scrollRow) scrollRow = row;
    if (row >= scrollRow + 4) scrollRow = row - 3;
    int maxScroll = lastRow > 3 ? lastRow - 3 : 0;
    if (scrollRow < 0) scrollRow = 0;
    return scrollRow > maxScroll ? maxScroll : scrollRow;
}

/// Status of a row-artwork load attempt.
enum class RowArtworkStatus {
    NotAttempted,   ///< Never tried (eligible for loading)
    Loaded,         ///< Decoded image is in RAM
    Failed          ///< Tried once and failed; do not retry
};

/// Per-key row-artwork tracking entry (B5d2a).
struct RowArtworkEntry {
    RowArtworkStatus status = RowArtworkStatus::NotAttempted;
    DecodedImage     image;               ///< Valid only when status == Loaded
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

/// Build the row-artwork identity key for a media item (B5d2a).
/// Format: "itemId:imageType:imageTag:WxH".
inline std::string buildRowArtworkKey(const MediaItem &item)
{
    DisplayArtwork artwork = displayArtworkForItem(item);
    if (!artwork.valid()) return {};
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%s:%s:%dx%d",
                  item.id.c_str(), imageTypeName(artwork.imageType), artwork.tag.c_str(), artwork.width, artwork.height);
    return std::string(buf);
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_LAYOUT_HPP
