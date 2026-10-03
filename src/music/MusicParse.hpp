#ifndef MIYOOFIN_MUSIC_PARSE_HPP
#define MIYOOFIN_MUSIC_PARSE_HPP

#include "MusicTypes.hpp"
#include <string>

namespace miyoofin {
namespace music {

/// Parsers for Jellyfin item JSON (one object each). Pure functions: the HTTP layer
/// lives in MusicApi.
Track parseTrack(const std::string& obj);
/// GET /Audio/{id}/Lyrics body -> lines in order (empty when there are none).
std::vector<LyricLine> parseLyrics(const std::string& body);
/// One "startMs<TAB>text" line per lyric line (the job result carries lyrics as text).
std::string packLyrics(const std::vector<LyricLine>& lines);
std::vector<LyricLine> unpackLyrics(const std::string& packed);
Album parseAlbum(const std::string& obj);
Artist parseArtist(const std::string& obj);
Playlist parsePlaylist(const std::string& obj);

/// Parses a `{"Items":[...],"TotalRecordCount":n,"StartIndex":n}` list response (also
/// accepts a bare `[...]` array, as /Items/Latest returns). `kind` selects the item
/// parser. False when the body is not a list.
enum class Kind
{
    Track,
    Album,
    Artist,
    Playlist
};
bool parseTrackPage(const std::string& body, Page<Track>& out);
bool parseAlbumPage(const std::string& body, Page<Album>& out);
bool parseArtistPage(const std::string& body, Page<Artist>& out);
bool parsePlaylistPage(const std::string& body, Page<Playlist>& out);

} // namespace music
} // namespace miyoofin

#endif
