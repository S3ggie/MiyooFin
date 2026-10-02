#ifndef MIYOOFIN_MUSIC_UI_STATE_HPP
#define MIYOOFIN_MUSIC_UI_STATE_HPP

#include <array>
#include <string>
#include <vector>

namespace miyoofin {

/// The five Music tabs, in header order.
enum class MusicTab
{
    Home,
    Library,
    Playlists,
    Downloads,
    Settings
};
constexpr int kMusicTabCount = 5;
/// Library sub-sections, switched with L2/R2.
constexpr int kLibrarySections = 3; // Artists, Albums, Songs

enum class MusicPaneKind
{
    Home,
    Artists,
    Albums,
    Songs,
    Playlists,
    Downloads,
    Settings,
    ArtistAlbums,
    AlbumTracks,
    PlaylistTracks
};

/// What survives leaving a page and coming back (and restarting the app): where the
/// cursor was and how to rebuild the page. Rows themselves are not kept; they reload from
/// the cache, so this stays tiny.
struct MusicFrame
{
    MusicPaneKind kind = MusicPaneKind::Home;
    std::string id;            // artist / album / playlist id for drill-down frames
    std::string title;         // shown in the breadcrumb
    std::string subtitle;      // artist / owner line for detail headers
    std::string artId, artTag; // cover for detail headers
    std::string selectedId;    // item under the cursor, restored by id after a reload
    int selected = 0, scroll = 0;
    char letter = 0; // alphabet filter on A-Z lists ('\0' = everything)
};

struct MusicTabState
{
    std::vector<MusicFrame> roots; // 1 frame per tab; Library has kLibrarySections
    int section = 0;               // active root (Library sub-section)
    std::vector<MusicFrame> drill; // pages opened on top of the active root
};

struct MusicUiState
{
    int activeTab = 0;
    std::array<MusicTabState, kMusicTabCount> tabs;

    /// Fresh state: every tab at the top of its root list.
    static MusicUiState defaults();
    std::string serialize() const;
    /// False (and `out` left at defaults) for anything that is not a valid state file.
    static bool parse(const std::string& text, MusicUiState& out);
};

} // namespace miyoofin

#endif
