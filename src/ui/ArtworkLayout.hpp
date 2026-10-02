#ifndef MIYOOFIN_ARTWORK_LAYOUT_HPP
#define MIYOOFIN_ARTWORK_LAYOUT_HPP

#include "../data/MediaItem.hpp"
#include "../image/ImageDecoder.hpp"
#include "../cache/ImageCache.hpp"
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <string>
#include <vector>

namespace miyoofin {

/// Returned by artworkBoxSize().
struct ArtworkBox
{
    int w;
    int h;
};

/// The single source of truth for display/cache artwork selection.
struct DisplayArtwork
{
    ImageType imageType = ImageType::Primary;
    std::string tag;
    int width = 0;
    int height = 0;
    /// Item the image belongs to; empty means the item itself (an episode that
    /// falls back to its show's poster names the series here).
    std::string itemId;
    bool valid() const
    {
        return !tag.empty() && width > 0 && height > 0;
    }
};

// Artwork is requested (and cached) at roughly twice the size it is shown at
// and box-filtered down, so cards stay crisp: episode thumbnails are 16:9 and
// everything else is a 2:3 poster.
inline constexpr int ARTWORK_POSTER_W = 128;
inline constexpr int ARTWORK_POSTER_H = 192;
inline constexpr int ARTWORK_THUMB_W = 192;
inline constexpr int ARTWORK_THUMB_H = 108;

/// `landscape` asks for wide art: any item with a Thumb image shows that (the
/// Continue Watching rail uses landscape cards, so a movie there gets its wide
/// still instead of a cropped poster). Episodes always prefer their Thumb.
inline DisplayArtwork displayArtworkForItem(const MediaItem& item, bool landscape = false)
{
    const bool episode = item.type == "episode";
    if (episode || landscape) {
        auto thumb = item.imageTags.find("Thumb");
        if (thumb != item.imageTags.end() && !thumb->second.empty())
            return {ImageType::Thumb, thumb->second, ARTWORK_THUMB_W, ARTWORK_THUMB_H};
    }
    auto primary = item.imageTags.find("Primary");
    if (primary != item.imageTags.end() && !primary->second.empty()) {
        // Episode stills are 16:9 even when only a Primary image exists.
        return {ImageType::Primary, primary->second, episode ? ARTWORK_THUMB_W : ARTWORK_POSTER_W,
                episode ? ARTWORK_THUMB_H : ARTWORK_POSTER_H};
    }
    if (episode) {
        auto series = item.imageTags.find("SeriesPrimary");
        if (series != item.imageTags.end() && !series->second.empty() && !item.seriesId.empty())
            return {ImageType::Primary, series->second, ARTWORK_POSTER_W, ARTWORK_POSTER_H,
                    item.seriesId};
    }
    return {};
}

inline const char* imageTypeName(ImageType type)
{
    return type == ImageType::Thumb ? "Thumb" : "Primary";
}

/// Splits an artwork identity key "itemId:Type:tag:WxH" (the layout of
/// HomeArtworkController::identityKey) back into its parts. False when malformed.
inline bool parseArtworkIdentityKey(const std::string& key, std::string& itemId, ImageType& type,
                                    std::string& tag, int& width, int& height)
{
    const std::size_t a = key.find(':');
    if (a == std::string::npos || a == 0)
        return false;
    const std::size_t b = key.find(':', a + 1);
    const std::size_t c = key.rfind(':');
    if (b == std::string::npos || c <= b)
        return false;
    const std::size_t x = key.find('x', c + 1);
    if (x == std::string::npos)
        return false;
    const std::string typeName = key.substr(a + 1, b - a - 1);
    if (typeName != "Thumb" && typeName != "Primary")
        return false;
    width = std::atoi(key.c_str() + c + 1);
    height = std::atoi(key.c_str() + x + 1);
    if (width <= 0 || height <= 0)
        return false;
    itemId = key.substr(0, a);
    type = typeName == "Thumb" ? ImageType::Thumb : ImageType::Primary;
    tag = key.substr(b + 1, c - b - 1);
    return true;
}

/// Maximum decoded row-artwork images kept in RAM (B5d2a).
static constexpr int ROW_ARTWORK_RAM_LIMIT = 64;
static constexpr int MOVIE_ARTWORK_DECODE_BUDGET = 4;
struct MovieArtworkRange
{
    int first;
    int lastExclusive;
};
inline MovieArtworkRange movieVisibleArtworkRange(int scrollRow, int itemCount)
{
    if (scrollRow < 0)
        scrollRow = 0;
    int first = scrollRow * 9;
    if (first > itemCount)
        first = itemCount;
    int last = first + 36;
    if (last > itemCount)
        last = itemCount;
    return {first, last};
}

// Pure Movies grid helpers.  The grid is deliberately index based so it is
// independent of SDL and straightforward to test.
inline int movieGridRow(int index)
{
    return index < 0 ? 0 : index / 9;
}
inline int movieGridColumn(int index)
{
    return index < 0 ? 0 : index % 9;
}
inline int moveMovieGrid(int index, int count, int deltaRow, int deltaCol)
{
    if (count <= 0)
        return 0;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    int row = movieGridRow(index) + deltaRow, col = movieGridColumn(index) + deltaCol;
    if (col < 0 || col >= 9 || row < 0)
        return index;
    int target = row * 9 + col;
    if (target >= count) {
        if (deltaRow > 0)
            target = count - 1; // nearest valid item in final row
        else
            return index;
    }
    return target;
}
inline int clampMovieGridScroll(int selected, int itemCount, int scrollRow)
{
    if (itemCount <= 0)
        return 0;
    int lastRow = (itemCount - 1) / 9;
    int row = movieGridRow(selected);
    if (row < scrollRow)
        scrollRow = row;
    if (row >= scrollRow + 4)
        scrollRow = row - 3;
    int maxScroll = lastRow > 3 ? lastRow - 3 : 0;
    if (scrollRow < 0)
        scrollRow = 0;
    return scrollRow > maxScroll ? maxScroll : scrollRow;
}

/// Status of a row-artwork load attempt.
enum class RowArtworkStatus
{
    NotAttempted, ///< Never tried (eligible for loading)
    Loaded,       ///< Decoded image is in RAM
    Failed        ///< Tried once and failed; do not retry
};

/// Per-key row-artwork tracking entry (B5d2a).
struct RowArtworkEntry
{
    RowArtworkStatus status = RowArtworkStatus::NotAttempted;
    std::shared_ptr<DecodedImage> image; ///< Valid only when status == Loaded
};

/// Return the selected-top-artwork box dimensions for a given media item.
///   movie / show / anything-else  →  64 × 96
///   episode                      → 128 × 72
inline ArtworkBox artworkBoxSize(const MediaItem& item)
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
inline int cardXPosition(const std::vector<MediaItem>& items, int ci, int startX = 4, int gap = 6)
{
    int x = startX;
    for (int i = 0; i < ci && i < (int)items.size(); ++i)
        x += artworkBoxSize(items[i]).w + gap;
    return x;
}

/// Total pixel width from startX through the right edge of the last card
/// (no trailing gap).
inline int totalRowWidth(const std::vector<MediaItem>& items, int startX = 4, int gap = 6)
{
    if (items.empty())
        return 0;
    int x = startX;
    for (const auto& item : items)
        x += artworkBoxSize(item).w + gap;
    return x - gap; // remove trailing gap
}

/// Horizontal pixel offset used when drawing one Home rail.  Only the focused
/// row scrolls; all other rows stay pinned to their left edge so scrolling a
/// longer rail cannot shift a shorter one.
inline int rowCardScrollOffset(int rowIdx, int activeRow, int activeScroll)
{
    return rowIdx == activeRow ? activeScroll : 0;
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
inline int clampCardScroll(const std::vector<MediaItem>& items, int activeCard, int curScroll,
                           int viewWidth, int startX = 4, int gap = 6)
{
    if (items.empty() || activeCard < 0 || activeCard >= (int)items.size())
        return 0;

    int scroll = curScroll;
    int cardX = startX;
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
    if (scroll < 0)
        scroll = 0;
    return scroll;
}

/// Home rail card geometry. Card SHAPE follows the content of the rail:
/// Continue Watching shows episode thumbnails (16:9), so it gets landscape
/// cards, four across with the fifth peeking in; poster rails (Recently Added,
/// ...) get 2:3 portrait cards, five across with the sixth peeking in. The peek
/// tells you the rail scrolls.
inline constexpr int HOME_RAIL_MARGIN = 16;
inline constexpr int HOME_RAIL_GAP = 8;
inline bool homeRailIsLandscape(const std::string& rowLabel)
{
    return rowLabel == "Continue Watching";
}
inline ArtworkBox homeRailCardSize(bool landscape)
{
    return landscape ? ArtworkBox{144, 81} : ArtworkBox{115, 172};
}

/// Horizontal scroll that keeps card `activeCard` fully inside
/// [HOME_RAIL_MARGIN, viewWidth - HOME_RAIL_MARGIN] for a rail of equal-width
/// cards; never negative, so the first card rests at the left margin.
inline int clampHomeCardScroll(int itemCount, int activeCard, int curScroll, int viewWidth,
                               int cardWidth)
{
    if (itemCount <= 0 || activeCard < 0 || activeCard >= itemCount)
        return 0;
    const int cardX = HOME_RAIL_MARGIN + activeCard * (cardWidth + HOME_RAIL_GAP);
    int scroll = curScroll;
    if (cardX - scroll < HOME_RAIL_MARGIN)
        scroll = cardX - HOME_RAIL_MARGIN;
    if (cardX + cardWidth - scroll > viewWidth - HOME_RAIL_MARGIN)
        scroll = cardX + cardWidth - (viewWidth - HOME_RAIL_MARGIN);
    return scroll < 0 ? 0 : scroll;
}

/// Build the row-artwork identity key for a media item (B5d2a).
/// Format: "itemId:imageType:imageTag:WxH".
inline std::string buildRowArtworkKey(const MediaItem& item, bool landscape = false)
{
    DisplayArtwork artwork = displayArtworkForItem(item, landscape);
    if (!artwork.valid())
        return {};
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s:%s:%s:%dx%d",
                  (artwork.itemId.empty() ? item.id : artwork.itemId).c_str(),
                  imageTypeName(artwork.imageType), artwork.tag.c_str(), artwork.width,
                  artwork.height);
    return std::string(buf);
}

} // namespace miyoofin

#endif // MIYOOFIN_ARTWORK_LAYOUT_HPP
