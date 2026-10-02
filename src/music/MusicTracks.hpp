#ifndef MIYOOFIN_MUSIC_TRACKS_HPP
#define MIYOOFIN_MUSIC_TRACKS_HPP

#include "MusicApi.hpp"
#include "MusicPlayer.hpp"
#include <functional>
#include <string>

namespace miyoofin {
struct Session;
namespace music {

/// Where a track's audio file comes from, in order of preference: a finished download, a
/// file already in the stream cache, or a fresh fetch into the stream cache. All file and
/// network work here belongs on a worker thread.
struct TrackSourceConfig
{
    std::string cacheDir; // stream cache directory
    AudioQuality quality; // streaming quality
    std::uint64_t cacheLimitBytes = 64ull * 1024 * 1024;
    /// A finished download of the track, or "" (set once downloads exist).
    std::function<std::string(const std::string& trackId)> downloadedPath;
    /// Fetches `trackId` into `destTmpPath`. Defaults to the Jellyfin download.
    std::function<bool(const std::string& trackId, const std::string& destTmpPath,
                       std::string& error, const std::atomic<bool>& cancelled)>
        fetch;
    /// True when the user is offline (manual offline mode): never touch the network.
    std::function<bool()> offline;
};

ResolvedTrack resolveTrack(const TrackSourceConfig& config, const Track& track,
                           const std::atomic<bool>& cancelled);

/// Looks like an MP3 stream (ID3 tag or MPEG frame sync) of plausible size.
bool looksLikeAudioFile(const std::string& path, std::string& error);

/// Deletes the oldest files in `dir` until it is under `limitBytes`, never touching `keep`.
void pruneCache(const std::string& dir, std::uint64_t limitBytes, const std::string& keep);

/// Deletes files in `dir` not modified in the last `seconds` (a cache clean-up that leaves what
/// is in use alone).
void pruneOlderThan(const std::string& dir, int seconds);

/// Production hooks for a session: fetch via the Jellyfin routes, report via the Sessions API.
TrackSourceConfig makeServerSource(const Session& session, std::string cacheDir,
                                   AudioQuality quality);

} // namespace music
} // namespace miyoofin

#endif
