#ifndef MIYOOFIN_MUSIC_TYPES_HPP
#define MIYOOFIN_MUSIC_TYPES_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace miyoofin {
namespace music {

/// Domain values for the music side of the app. No SDL or UI state in here.
struct Track
{
    std::string id, title, album, albumId, artist, artistId, albumArtist;
    std::string entryId;  // PlaylistItemId: this track's slot in a playlist (needed to remove it)
    std::string imageTag; // the track's own Primary image, usually empty
    std::string albumImageTag; // the album's Primary image (what art shows for a track)
    int trackNumber = 0, discNumber = 0;
    std::int64_t runTimeTicks = 0; // 100 ns units, like Jellyfin
    bool favorite = false;
    int durationSeconds() const
    {
        return static_cast<int>(runTimeTicks / 10000000);
    }
    /// The item whose Primary image shows this track's cover art.
    const std::string& artId() const
    {
        return imageTag.empty() && !albumId.empty() ? albumId : id;
    }
    const std::string& artTag() const
    {
        return imageTag.empty() && !albumId.empty() ? albumImageTag : imageTag;
    }
};

struct Album
{
    std::string id, title, artist, artistId, imageTag;
    int year = 0, trackCount = 0;
    std::int64_t runTimeTicks = 0;
};

struct Artist
{
    std::string id, name, imageTag;
};

struct Playlist
{
    std::string id, title, imageTag;
    int trackCount = 0;
    std::int64_t runTimeTicks = 0;
};

/// One server page of a listing.
template <typename T> struct Page
{
    std::vector<T> items;
    int startIndex = 0;
    int total = 0; // whole result-set size (TotalRecordCount)
    bool hasMore() const
    {
        return startIndex + static_cast<int>(items.size()) < total;
    }
};

inline std::string formatDuration(int seconds)
{
    if (seconds < 0)
        seconds = 0;
    char buf[16];
    if (seconds >= 3600)
        std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60,
                      seconds % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
    return buf;
}

} // namespace music
} // namespace miyoofin

#endif
