#pragma once

#include "MusicScreen.hpp"
#include "../../music/MusicParse.hpp"
#include <SDL2/SDL.h>
#include <cstdint>
#include <string>

namespace miyoofin {
namespace music_screen_detail {
// File-local helpers and constants shared by the MusicScreen*.cpp implementation units (the
// class is one; its definitions are split by responsibility). Rendering keeps its own copies.

inline constexpr const char* kSettingsFile = "music-settings.txt";
inline constexpr int kRowHeight = 44;
inline constexpr int kGridCols = 4, kGridCellH = 156;
inline constexpr int kPrefetchRows = 12;
inline constexpr Uint32 kStateSaveMs = 2000;
inline constexpr std::size_t kMaxCovers = 80;

inline constexpr int kMenuPlay = 1, kMenuShuffle = 2, kMenuPlayNext = 3, kMenuAppend = 4,
                     kMenuGoAlbum = 5, kMenuGoArtist = 6, kMenuDownload = 7, kMenuRemove = 8,
                     kMenuRetry = 9, kMenuDownloadPage = 10, kMenuPlayDownloaded = 11,
                     kMenuShuffleDownloaded = 12, kMenuAddToPlaylist = 13,
                     kMenuRemoveFromPlaylist = 14, kMenuDeletePlaylist = 15, kMenuSync = 16,
                     kMenuFavorite = 17;
inline constexpr const char* kDownloadPrefix = "dl:";
inline constexpr const char* kNewPlaylistId = "__newplaylist__";

inline std::string readText(const std::string& path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline bool writeText(const std::string& path, const std::string& text)
{
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << text;
        if (!out.good())
            return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline std::string minutes(std::int64_t ticks)
{
    const int m = static_cast<int>(ticks / 10000000 / 60);
    return std::to_string(m) + " min";
}

inline MusicRow headingRow(const std::string& text)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Heading;
    r.title = text;
    return r;
}

inline MusicRow trackRow(const music::Track& t, bool numbered)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Track;
    r.id = t.id;
    r.track = t;
    r.title = (numbered && t.trackNumber > 0 ? std::to_string(t.trackNumber) + ". " : "") + t.title;
    // On an album's page the artist is only worth a line when it differs from the album's.
    r.subtitle = numbered && t.artist == t.albumArtist ? std::string() : t.artist;
    if (!t.album.empty() && !numbered)
        r.subtitle += (r.subtitle.empty() ? "" : " - ") + t.album;
    r.right = music::formatDuration(t.durationSeconds());
    r.artId = t.artId();
    r.artTag = t.artTag();
    return r;
}

inline MusicRow albumRow(const music::Album& a)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Album;
    r.id = a.id;
    r.album = a;
    r.title = a.title;
    r.subtitle = a.artist;
    if (a.year > 0)
        r.right = std::to_string(a.year);
    r.artId = a.id;
    r.artTag = a.imageTag;
    return r;
}

inline MusicRow artistRow(const music::Artist& a)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Artist;
    r.id = a.id;
    r.artist = a;
    r.title = a.name;
    r.artId = a.id;
    r.artTag = a.imageTag;
    return r;
}

inline MusicRow playlistRow(const music::Playlist& p)
{
    MusicRow r;
    r.kind = MusicRow::Kind::Playlist;
    r.id = p.id;
    r.playlist = p;
    r.title = p.title;
    r.subtitle = std::to_string(p.trackCount) + (p.trackCount == 1 ? " track" : " tracks");
    r.right = p.runTimeTicks > 0 ? minutes(p.runTimeTicks) : "";
    r.artId = p.id;
    r.artTag = p.imageTag;
    return r;
}

inline bool isAlphabetical(MusicPaneKind k)
{
    return k == MusicPaneKind::Artists || k == MusicPaneKind::Albums || k == MusicPaneKind::Songs;
}

inline constexpr const char* kResumeId = "__resume__";

} // namespace music_screen_detail
} // namespace miyoofin
