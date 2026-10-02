#include "MusicParse.hpp"
#include "../net/JellyfinApi.hpp"

namespace miyoofin {
namespace music {

namespace {

using J = JellyfinApi;

std::int64_t int64Field(const std::string& obj, const std::string& key)
{
    const std::string v = J::jsonRawValue(obj, key);
    if (v.empty() || v == "null")
        return 0;
    try {
        return std::stoll(v);
    } catch (...) {
        return 0;
    }
}

std::string primaryTag(const std::string& obj)
{
    const std::string tags = J::jsonRawValue(obj, "ImageTags");
    if (tags.empty() || tags[0] != '{')
        return {};
    return J::jsonStringField(tags, "Primary");
}

// First entry of an array of {"Name":..,"Id":..} objects.
void firstNamed(const std::string& obj, const char* key, std::string& name, std::string& id)
{
    const std::string raw = J::jsonRawValue(obj, key);
    if (raw.empty() || raw[0] != '[')
        return;
    const auto entries = J::jsonExtractArray(obj, key);
    if (entries.empty())
        return;
    name = J::jsonStringField(entries[0], "Name");
    id = J::jsonStringField(entries[0], "Id");
}

bool listItems(const std::string& body, std::vector<std::string>& items, int& start, int& total)
{
    std::size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    if (body[first] == '[') {
        // Bare array: wrap it so jsonExtractArray can find it.
        items = J::splitJsonArrayContent(body.substr(first + 1, body.rfind(']') - first - 1));
        start = 0;
        total = static_cast<int>(items.size());
        return body.rfind(']') != std::string::npos;
    }
    const std::string raw = J::jsonRawValue(body, "Items");
    if (raw.empty() || raw[0] != '[')
        return false;
    items = J::jsonExtractArray(body, "Items");
    start = J::jsonIntField(body, "StartIndex");
    total = J::jsonIntField(body, "TotalRecordCount");
    if (total < start + static_cast<int>(items.size()))
        total = start + static_cast<int>(items.size());
    return true;
}

template <typename T, typename Parse>
bool parsePage(const std::string& body, Page<T>& out, Parse parse)
{
    std::vector<std::string> items;
    out = {};
    if (!listItems(body, items, out.startIndex, out.total))
        return false;
    for (const auto& raw : items) {
        T value = parse(raw);
        if (!value.id.empty())
            out.items.push_back(std::move(value));
    }
    return true;
}

} // namespace

Track parseTrack(const std::string& obj)
{
    Track t;
    t.id = J::jsonStringField(obj, "Id");
    t.title = J::jsonStringField(obj, "Name");
    t.album = J::jsonStringField(obj, "Album");
    t.albumId = J::jsonStringField(obj, "AlbumId");
    t.albumArtist = J::jsonStringField(obj, "AlbumArtist");
    firstNamed(obj, "ArtistItems", t.artist, t.artistId);
    if (t.artist.empty())
        t.artist = t.albumArtist;
    t.entryId = J::jsonStringField(obj, "PlaylistItemId");
    t.imageTag = primaryTag(obj);
    t.albumImageTag = J::jsonStringField(obj, "AlbumPrimaryImageTag");
    t.trackNumber = J::jsonIntField(obj, "IndexNumber");
    t.discNumber = J::jsonIntField(obj, "ParentIndexNumber");
    t.runTimeTicks = int64Field(obj, "RunTimeTicks");
    const std::string userData = J::jsonRawValue(obj, "UserData");
    if (!userData.empty() && userData[0] == '{')
        t.favorite = J::jsonBoolField(userData, "IsFavorite");
    return t;
}

Album parseAlbum(const std::string& obj)
{
    Album a;
    a.id = J::jsonStringField(obj, "Id");
    a.title = J::jsonStringField(obj, "Name");
    a.artist = J::jsonStringField(obj, "AlbumArtist");
    std::string artistName;
    firstNamed(obj, "AlbumArtists", artistName, a.artistId);
    if (a.artist.empty())
        a.artist = artistName;
    a.imageTag = primaryTag(obj);
    a.year = J::jsonIntField(obj, "ProductionYear");
    a.trackCount = J::jsonIntField(obj, "ChildCount");
    if (a.trackCount == 0)
        a.trackCount = J::jsonIntField(obj, "SongCount");
    a.runTimeTicks = int64Field(obj, "RunTimeTicks");
    return a;
}

Artist parseArtist(const std::string& obj)
{
    Artist a;
    a.id = J::jsonStringField(obj, "Id");
    a.name = J::jsonStringField(obj, "Name");
    a.imageTag = primaryTag(obj);
    return a;
}

Playlist parsePlaylist(const std::string& obj)
{
    Playlist p;
    p.id = J::jsonStringField(obj, "Id");
    p.title = J::jsonStringField(obj, "Name");
    p.imageTag = primaryTag(obj);
    p.trackCount = J::jsonIntField(obj, "ChildCount");
    p.runTimeTicks = int64Field(obj, "RunTimeTicks");
    return p;
}

bool parseTrackPage(const std::string& body, Page<Track>& out)
{
    return parsePage(body, out, parseTrack);
}
bool parseAlbumPage(const std::string& body, Page<Album>& out)
{
    return parsePage(body, out, parseAlbum);
}
bool parseArtistPage(const std::string& body, Page<Artist>& out)
{
    return parsePage(body, out, parseArtist);
}
bool parsePlaylistPage(const std::string& body, Page<Playlist>& out)
{
    return parsePage(body, out, parsePlaylist);
}

} // namespace music
} // namespace miyoofin
