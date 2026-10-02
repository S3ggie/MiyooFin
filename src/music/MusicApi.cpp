#include "MusicApi.hpp"
#include "MusicParse.hpp"
#include "../net/HttpClient.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/ArtworkUrl.hpp"
#include "../net/RouteRequest.hpp"
#include "../net/Session.hpp"

namespace miyoofin {
namespace music {

namespace {

constexpr const char* kImageFields = "&EnableImageTypes=Primary&ImageTypeLimit=1";

std::string percentEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string pageParams(const ListingRequest& r)
{
    return "&StartIndex=" + std::to_string(r.start < 0 ? 0 : r.start) +
           "&Limit=" + std::to_string(r.limit < 1 ? 1 : r.limit);
}

// Alphabetical filtering: a letter keeps names starting with it, '#' keeps everything
// that does not start with a letter.
std::string letterParams(const ListingRequest& r)
{
    if (r.letter >= 'A' && r.letter <= 'Z')
        return std::string("&NameStartsWith=") + r.letter;
    if (r.letter == '#')
        return "&NameLessThan=A";
    return {};
}

bool fetchBody(const Connection& c, const std::string& url, std::string& body, std::string& error,
               const std::atomic<bool>* cancelled)
{
    HttpClient client;
    client.setTimeoutSec(20);
    HttpResponse response;
    if (!client.perform("GET", url, JellyfinApi::buildAuthHeaders(c.accessToken, c.deviceId), {},
                        response, error, cancelled)) {
        if (error.empty())
            error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        error = response.status == 401 || response.status == 403
                    ? "Unauthorized"
                    : "Request failed (HTTP " + std::to_string(response.status) + ")";
        return false;
    }
    body = std::move(response.body);
    return true;
}

template <typename T, typename Parse>
bool fetchPage(const Connection& c, const ListingRequest& r, Page<T>& out, std::string& error,
               const std::atomic<bool>* cancelled, Parse parse)
{
    std::string body;
    if (!fetchBody(c, buildListingUrl(c.baseUrl, c.userId, r), body, error, cancelled))
        return false;
    if (!parse(body, out)) {
        error = "Malformed music response";
        return false;
    }
    if (out.startIndex == 0)
        out.startIndex = r.start;
    return true;
}

} // namespace

bool onRoute(const Session& session, std::string& error,
             const std::function<bool(const Connection&)>& operation)
{
    return RouteRequest(session).run(
        [&](const std::string& base) {
            Connection c{base, session.accessToken, session.userId, session.deviceId};
            return operation(c);
        },
        error);
}

std::string ListingRequest::key() const
{
    static const char* names[] = {"artists",       "albums",        "songs",
                                  "playlists",     "recent-albums", "recent-played",
                                  "artist-albums", "album-tracks",  "playlist-tracks"};
    std::string k = names[static_cast<int>(kind)];
    if (!parentId.empty())
        k += "-" + parentId;
    if (letter)
        k += std::string("-") + letter;
    return k;
}

std::string buildListingUrl(const std::string& baseUrl, const std::string& userId,
                            const ListingRequest& r)
{
    const std::string user = userId.empty() ? std::string() : "UserId=" + percentEncode(userId);
    const std::string items = baseUrl + "/Users/" + percentEncode(userId) + "/Items?Recursive=true";
    switch (r.kind) {
    case Listing::Artists:
        return baseUrl + "/Artists/AlbumArtists?" + user + "&SortBy=SortName&SortOrder=Ascending" +
               kImageFields + letterParams(r) + pageParams(r);
    case Listing::Albums:
        return items +
               "&IncludeItemTypes=MusicAlbum&SortBy=SortName&SortOrder=Ascending"
               "&Fields=ChildCount,AlbumArtists" +
               kImageFields + letterParams(r) + pageParams(r);
    case Listing::Songs:
        return items +
               "&IncludeItemTypes=Audio&SortBy=SortName&SortOrder=Ascending"
               "&Fields=AlbumArtists" +
               kImageFields + letterParams(r) + pageParams(r);
    case Listing::Playlists:
        return items +
               "&IncludeItemTypes=Playlist&MediaTypes=Audio&SortBy=SortName"
               "&SortOrder=Ascending&Fields=ChildCount" +
               kImageFields + pageParams(r);
    case Listing::RecentAlbums:
        return items +
               "&IncludeItemTypes=MusicAlbum&SortBy=DateCreated&SortOrder=Descending"
               "&Fields=ChildCount,AlbumArtists" +
               kImageFields + pageParams(r);
    case Listing::RecentlyPlayed:
        return items +
               "&IncludeItemTypes=Audio&Filters=IsPlayed&SortBy=DatePlayed"
               "&SortOrder=Descending&Fields=AlbumArtists" +
               kImageFields + pageParams(r);
    case Listing::ArtistAlbums:
        return items + "&IncludeItemTypes=MusicAlbum&AlbumArtistIds=" + percentEncode(r.parentId) +
               "&SortBy=ProductionYear,SortName&SortOrder=Descending"
               "&Fields=ChildCount,AlbumArtists" +
               kImageFields + pageParams(r);
    case Listing::AlbumTracks:
        return items + "&IncludeItemTypes=Audio&ParentId=" + percentEncode(r.parentId) +
               "&SortBy=ParentIndexNumber,IndexNumber,SortName&SortOrder=Ascending"
               "&Fields=AlbumArtists" +
               kImageFields + pageParams(r);
    case Listing::PlaylistTracks:
        return baseUrl + "/Playlists/" + percentEncode(r.parentId) + "/Items?" + user +
               "&Fields=AlbumArtists" + kImageFields + pageParams(r);
    }
    return {};
}

std::string buildTrackUrl(const std::string& baseUrl, const std::string& trackId,
                          const AudioQuality& quality)
{
    if (quality.kbps <= 0)
        return baseUrl + "/Items/" + percentEncode(trackId) + "/Download";
    return baseUrl + "/Audio/" + percentEncode(trackId) +
           "/stream.mp3?audioCodec=mp3&static=false&audioBitRate=" +
           std::to_string(quality.kbps * 1000) + "&maxAudioChannels=2";
}

const char* trackExtension(const AudioQuality& quality)
{
    return quality.kbps <= 0 ? "audio" : "mp3";
}

std::string buildCoverUrl(const std::string& baseUrl, const std::string& itemId,
                          const std::string& tag, int size)
{
    return buildImageUrl(baseUrl, itemId, ImageType::Primary, tag, size, size);
}

bool fetchTracks(const Connection& c, const ListingRequest& r, Page<Track>& out, std::string& error,
                 const std::atomic<bool>* cancelled)
{
    return fetchPage(c, r, out, error, cancelled, parseTrackPage);
}
bool fetchAlbums(const Connection& c, const ListingRequest& r, Page<Album>& out, std::string& error,
                 const std::atomic<bool>* cancelled)
{
    return fetchPage(c, r, out, error, cancelled, parseAlbumPage);
}
bool fetchArtists(const Connection& c, const ListingRequest& r, Page<Artist>& out,
                  std::string& error, const std::atomic<bool>* cancelled)
{
    return fetchPage(c, r, out, error, cancelled, parseArtistPage);
}
bool fetchPlaylists(const Connection& c, const ListingRequest& r, Page<Playlist>& out,
                    std::string& error, const std::atomic<bool>* cancelled)
{
    return fetchPage(c, r, out, error, cancelled, parsePlaylistPage);
}

bool fetchCover(const Connection& c, const std::string& itemId, const std::string& tag, int size,
                std::vector<unsigned char>& jpeg, std::string& error,
                const std::atomic<bool>* cancelled)
{
    HttpClient client;
    client.setTimeoutSec(20);
    BinaryHttpResponse response;
    if (!client.getBinary(buildCoverUrl(c.baseUrl, itemId, tag, size),
                          JellyfinApi::buildAuthHeaders(c.accessToken, c.deviceId), response, error,
                          1024 * 1024, cancelled)) {
        if (error.empty())
            error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        error = "No cover art";
        return false;
    }
    jpeg = std::move(response.data);
    return !jpeg.empty();
}

bool downloadTrack(const Connection& c, const std::string& trackId, const AudioQuality& quality,
                   const std::string& destTmpPath, std::string& error,
                   const std::atomic<bool>* cancelled)
{
    HttpClient client;
    return client.downloadToFile(buildTrackUrl(c.baseUrl, trackId, quality),
                                 JellyfinApi::buildAuthHeaders(c.accessToken, c.deviceId),
                                 destTmpPath, error, nullptr, {}, cancelled, 600, 15, 0);
}

bool reportPlayback(const Connection& c, ReportKind kind, const std::string& trackId,
                    std::int64_t positionTicks, bool paused, const std::string& playSessionId,
                    std::string& error)
{
    const char* path = kind == ReportKind::Start      ? "/Sessions/Playing"
                       : kind == ReportKind::Progress ? "/Sessions/Playing/Progress"
                                                      : "/Sessions/Playing/Stopped";
    std::string body = "{\"ItemId\":\"" + trackId + "\",\"PositionTicks\":" +
                       std::to_string(positionTicks < 0 ? 0 : positionTicks) +
                       ",\"PlaySessionId\":\"" + playSessionId + "\"";
    if (kind != ReportKind::Stopped)
        body += std::string(",\"CanSeek\":true,\"IsPaused\":") + (paused ? "true" : "false") +
                ",\"IsMuted\":false";
    else
        body += ",\"Failed\":false";
    body += "}";
    HttpClient client;
    client.setTimeoutSec(10);
    HttpResponse response;
    auto headers = JellyfinApi::buildAuthHeaders(c.accessToken, c.deviceId);
    headers.push_back("Content-Type: application/json");
    if (!client.post(c.baseUrl + path, headers, body, response, error)) {
        if (error.empty())
            error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        error = "Report failed (HTTP " + std::to_string(response.status) + ")";
        return false;
    }
    return true;
}

bool markPlayed(const Connection& c, const std::string& trackId, const std::string& isoTime,
                bool& gone, std::string& error)
{
    gone = false;
    HttpClient client;
    client.setTimeoutSec(10);
    HttpResponse response;
    auto headers = JellyfinApi::buildAuthHeaders(c.accessToken, c.deviceId);
    if (!client.perform("POST",
                        c.baseUrl + "/Users/" + percentEncode(c.userId) + "/PlayedItems/" +
                            percentEncode(trackId) + "?DatePlayed=" + percentEncode(isoTime),
                        headers, {}, response, error)) {
        if (error.empty())
            error = "Could not reach server";
        return false;
    }
    if (response.status == 404) {
        gone = true;
        return false;
    }
    if (!response.ok()) {
        error = "Mark played failed (HTTP " + std::to_string(response.status) + ")";
        return false;
    }
    return true;
}

} // namespace music
} // namespace miyoofin
