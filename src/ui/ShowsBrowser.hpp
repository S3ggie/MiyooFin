#ifndef MIYOOFIN_SHOWS_BROWSER_HPP
#define MIYOOFIN_SHOWS_BROWSER_HPP
#include "../data/TitleOrganization.hpp"
#include "../cache/LibraryCache.hpp"
#include <algorithm>
#include <map>
namespace miyoofin {
static constexpr int SHOWS_GRID_COLUMNS = 4, SHOWS_GRID_ROWS = 3;
inline bool asciiEqualsAnime(const std::string& s)
{
    return asciiCaseInsensitiveCompare(s, "Anime") == 0;
}
inline bool libraryNameContainsAnimeToken(const std::string& s)
{
    for (size_t i = 0; i < s.size();) {
        while (i < s.size() && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
            ++i;
        size_t b = i;
        while (i < s.size() && ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')))
            ++i;
        if (i > b &&
            asciiCaseInsensitiveCompare(std::string_view(s).substr(b, i - b), "anime") == 0)
            return true;
    }
    return false;
}
inline bool isAnimeSeries(const std::string& viewName, const MediaItem& item)
{
    if (libraryNameContainsAnimeToken(viewName))
        return true;
    for (const auto& g : item.genres)
        if (asciiEqualsAnime(g))
            return true;
    return false;
}
inline bool isAnimeSeries(const CachedLibraryView& view, const MediaItem& item)
{
    return isAnimeSeries(view.name, item);
}
struct ShowsPresentation
{
    std::vector<MediaItem> shows, anime;
};
inline ShowsPresentation makeShowsPresentation(const std::vector<CachedLibraryView>& views)
{
    std::map<std::string, size_t> normal, anime;
    ShowsPresentation out;
    for (const auto& v : views)
        for (const auto& i : v.items) {
            bool a = isAnimeSeries(v, i);
            auto ni = normal.find(i.id), ai = anime.find(i.id);
            if (a) {
                if (ai == anime.end()) {
                    if (ni != normal.end()) {
                        out.shows.erase(out.shows.begin() + ni->second);
                        normal.clear();
                        for (size_t n = 0; n < out.shows.size(); ++n)
                            normal[out.shows[n].id] = n;
                    }
                    anime[i.id] = out.anime.size();
                    out.anime.push_back(i);
                }
            } else if (ni == normal.end() && ai == anime.end()) {
                normal[i.id] = out.shows.size();
                out.shows.push_back(i);
            }
        }
    std::sort(out.shows.begin(), out.shows.end(), organizationalLess);
    std::sort(out.anime.begin(), out.anime.end(), organizationalLess);
    return out;
}
inline int moveShowsGrid(int index, int count, int dr, int dc, int columns = 4)
{
    if (count <= 0)
        return 0;
    index = std::max(0, std::min(index, count - 1));
    int r = index / columns + dr, c = index % columns + dc;
    if (c < 0 || c >= columns || r < 0)
        return index;
    int t = r * columns + c;
    return t >= count ? (dr > 0 ? count - 1 : index) : t;
}
/// Remembers the column a vertical walk started in, so a short last row does not
/// drag the cursor into another column. Valid only while the selection is still
/// where the last vertical move left it.
struct StickyColumn
{
    int index = -1, column = 0;
};
/// One row up/down. The column is kept across rows; a short last row clamps to its
/// last item, and going back up returns to the original column. No move past the
/// first or last row.
inline int moveGridVertical(int index, int count, int columns, int dr, StickyColumn& sticky)
{
    if (count <= 0 || columns <= 0)
        return 0;
    index = std::max(0, std::min(index, count - 1));
    if (sticky.index != index)
        sticky.column = index % columns;
    const int row = index / columns + dr, lastRow = (count - 1) / columns;
    int target = index;
    if (row >= 0 && row <= lastRow)
        target = std::min(row * columns + sticky.column, count - 1);
    sticky.index = target;
    return target;
}
inline int clampShowsGridScroll(int selected, int count, int scroll, int columns = 4)
{
    if (count <= 0)
        return 0;
    selected = std::max(0, std::min(selected, count - 1));
    int row = selected / columns, last = (count - 1) / columns, max = std::max(0, last - 2);
    scroll = std::max(0, std::min(scroll, max));
    if (row < scroll)
        scroll = row;
    if (row >= scroll + 3)
        scroll = row - 2;
    return scroll;
}
/// How many leading window items to drop (at least `minRemove`) so that the number of
/// visible ones dropped is a whole number of grid rows; `visible[i]` says whether window
/// item i is shown in the grid. Keeps every remaining item in its column.
inline std::size_t alignedWindowTrim(const std::vector<bool>& visible, std::size_t minRemove,
                                     std::size_t columns)
{
    std::size_t remove = std::min(minRemove, visible.size()), shown = 0;
    for (std::size_t i = 0; i < remove; ++i)
        shown += visible[i] ? 1 : 0;
    while (columns && shown % columns != 0 && remove < visible.size())
        shown += visible[remove++] ? 1 : 0;
    return remove;
}
inline int closestShowsGridIndex(int source, int targetCount)
{
    if (targetCount <= 0)
        return 0;
    int row = source / 4, col = source % 4;
    int t = row * 4 + col;
    return std::min(t, targetCount - 1);
}
// Cross a continuous eight-column Shows/Anime row.  Crossing always lands on
// the edge column of the target half, retaining the closest corresponding row.
inline int crossShowsGridIndex(int source, int targetCount, bool toAnime)
{
    if (targetCount <= 0)
        return 0;
    int row = std::max(0, source) / 4;
    int target = row * 4 + (toAnime ? 0 : 3);
    int rowFirst = row * 4, rowLast = std::min(targetCount - 1, rowFirst + 3);
    if (rowFirst <= targetCount - 1)
        return std::max(rowFirst, std::min(target, rowLast));
    return targetCount - 1;
}
}
#endif
