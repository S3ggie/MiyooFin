#ifndef MIYOOFIN_MUSIC_API_HPP
#define MIYOOFIN_MUSIC_API_HPP

#include "MusicTypes.hpp"
#include <atomic>
#include <string>
#include <vector>

#include <functional>

namespace miyoofin {
struct Session;
namespace music {

/// What a request needs to reach the server. Built per attempt (the route can differ).
struct Connection
{
    std::string baseUrl, accessToken, userId, deviceId;
};

enum class Listing
{
    Artists,        // album artists, A-Z
    Albums,         // every album, A-Z
    Songs,          // every song, A-Z
    Playlists,      // audio playlists
    RecentAlbums,   // newest albums first
    RecentlyPlayed, // tracks, most recently played first
    ArtistAlbums,   // parentId = artist id
    AlbumTracks,    // parentId = album id
    PlaylistTracks  // parentId = playlist id
};

struct ListingRequest
{
    Listing kind = Listing::Albums;
    std::string parentId;
    int start = 0, limit = 50;
    char letter = 0; // 'A'..'Z' or '#': only names starting with it (alphabetical listings)
    /// Stable cache/identity key: same key == same listing.
    std::string key() const;
};

/// Runs `operation` against the session's LAN route first, then the public one (the same
/// fallback every other request uses). `error` carries the last failure.
bool onRoute(const Session& session, std::string& error,
             const std::function<bool(const Connection&)>& operation);

/// Streaming quality for tracks played or downloaded from the server.
struct AudioQuality
{
    int kbps = 192; // 0 = the original file, untouched
};

/// Pure URL builders (unit-tested). Access tokens are never part of a URL: callers send
/// them as headers.
std::string buildListingUrl(const std::string& baseUrl, const std::string& userId,
                            const ListingRequest& request);
std::string buildTrackUrl(const std::string& baseUrl, const std::string& trackId,
                          const AudioQuality& quality);
std::string buildCoverUrl(const std::string& baseUrl, const std::string& itemId,
                          const std::string& tag, int size);
/// File extension (without dot) the downloaded track gets for a quality setting.
const char* trackExtension(const AudioQuality& quality);

bool fetchTracks(const Connection& c, const ListingRequest& r, Page<Track>& out, std::string& error,
                 const std::atomic<bool>* cancelled = nullptr);
bool fetchAlbums(const Connection& c, const ListingRequest& r, Page<Album>& out, std::string& error,
                 const std::atomic<bool>* cancelled = nullptr);
bool fetchArtists(const Connection& c, const ListingRequest& r, Page<Artist>& out,
                  std::string& error, const std::atomic<bool>* cancelled = nullptr);
bool fetchPlaylists(const Connection& c, const ListingRequest& r, Page<Playlist>& out,
                    std::string& error, const std::atomic<bool>* cancelled = nullptr);

/// Cover art bytes (JPEG) for an item.
bool fetchCover(const Connection& c, const std::string& itemId, const std::string& tag, int size,
                std::vector<unsigned char>& jpeg, std::string& error,
                const std::atomic<bool>* cancelled = nullptr);

/// Streams a track to `destTmpPath` (HttpClient::downloadToFile semantics: partial file kept
/// on cancel/failure). Never call from the UI thread.
bool downloadTrack(const Connection& c, const std::string& trackId, const AudioQuality& quality,
                   const std::string& destTmpPath, std::string& error,
                   const std::atomic<bool>* cancelled = nullptr);

enum class ReportKind
{
    Start,
    Progress,
    Stopped
};
/// Playback reporting for the Jellyfin "now playing" / play-count bookkeeping.
bool reportPlayback(const Connection& c, ReportKind kind, const std::string& trackId,
                    std::int64_t positionTicks, bool paused, const std::string& playSessionId,
                    std::string& error);

/// Playlist changes (music playlists only). All of them need the server.
std::string buildCreatePlaylistBody(const std::string& name, const std::string& userId,
                                    const std::vector<std::string>& trackIds);
std::string buildAddToPlaylistUrl(const std::string& baseUrl, const std::string& userId,
                                  const std::string& playlistId,
                                  const std::vector<std::string>& trackIds);
std::string buildRemoveFromPlaylistUrl(const std::string& baseUrl, const std::string& playlistId,
                                       const std::vector<std::string>& entryIds);
bool createPlaylist(const Connection& c, const std::string& name,
                    const std::vector<std::string>& trackIds, std::string& newId,
                    std::string& error);
bool addToPlaylist(const Connection& c, const std::string& playlistId,
                   const std::vector<std::string>& trackIds, std::string& error);
bool removeFromPlaylist(const Connection& c, const std::string& playlistId,
                        const std::vector<std::string>& entryIds, std::string& error);
bool deletePlaylist(const Connection& c, const std::string& playlistId, std::string& error);

/// Marks a track as played at `isoTime` (offline plays synced later). `gone` is set when the
/// server no longer has the item, so the caller can drop the entry for good.
bool markPlayed(const Connection& c, const std::string& trackId, const std::string& isoTime,
                bool& gone, std::string& error);

} // namespace music
} // namespace miyoofin

#endif
