#ifndef MIYOOFIN_MUSIC_DOWNLOADS_HPP
#define MIYOOFIN_MUSIC_DOWNLOADS_HPP

#include "MusicTypes.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {
namespace music {

/// An album, a playlist or a single track that was downloaded for offline listening.
struct DownloadCollection
{
    std::string id;   // album / playlist id, or "track-<id>"
    std::string kind; // "album" | "playlist" | "track"
    std::string name, artist, artId, artTag;
    std::vector<std::string> trackIds;
};

struct DownloadStatus
{
    DownloadCollection collection;
    int done = 0, total = 0;
    std::uint64_t bytes = 0;
    bool active = false; // a track of it is downloading right now
    bool failed = false;
    std::string error;
};

/// Offline music: audio files saved under `dir/tracks`, an index of what belongs to which
/// collection, and one worker thread that downloads the queue one track at a time. Separate
/// from the video DownloadManager on purpose: tracks are small single files, there is no
/// transcode state to resume, and a bug here must never touch video downloads.
class MusicDownloads
{
  public:
    struct Hooks
    {
        /// Downloads `trackId` to `destTmpPath` (blocking; honours `cancelled`).
        std::function<bool(const std::string& trackId, const std::string& destTmpPath,
                           std::string& error, const std::atomic<bool>& cancelled)>
            fetch;
        /// True while the user is in offline mode: nothing is downloaded then.
        std::function<bool()> offline;
        /// Bytes free on the storage holding `dir`; 0 = unknown (no check).
        std::function<std::uint64_t()> freeBytes;
    };

    MusicDownloads(std::string dir, Hooks hooks);
    ~MusicDownloads();
    MusicDownloads(const MusicDownloads&) = delete;
    MusicDownloads& operator=(const MusicDownloads&) = delete;

    /// Queues a collection for download. Tracks already on disk are not fetched again.
    void enqueue(DownloadCollection collection, const std::vector<Track>& tracks);
    /// Forgets a collection; files no other collection uses are deleted (by the worker).
    void removeCollection(const std::string& id);
    /// Puts a failed collection back in the queue.
    void retry(const std::string& id);

    /// Path of a finished download of the track, or "".
    std::string pathFor(const std::string& trackId) const;
    bool hasTrack(const std::string& trackId) const;
    bool hasCollection(const std::string& id) const;
    /// Finished tracks of a collection, in collection order.
    std::vector<Track> tracksOf(const std::string& id) const;
    std::vector<DownloadStatus> snapshot() const;
    std::uint64_t totalBytes() const;
    /// Changes whenever the downloads change; cheap to poll from the UI thread.
    std::uint64_t revision() const
    {
        return m_revision.load();
    }
    /// Test hook: true when nothing is queued or downloading.
    bool idle() const;

    static constexpr int kMaxAttempts = 3;
    /// Never fill the card: stop when less than this is left.
    static constexpr std::uint64_t kReserveBytes = 200ull * 1024 * 1024;
    /// Seconds to wait before attempt n (1-based). Overridable so tests do not sleep.
    std::function<int(int attempt)> backoffSeconds = [](int attempt) { return attempt * 4; };

  private:
    struct TrackEntry
    {
        Track track;
        std::uint64_t bytes = 0;
        bool done = false;
        int attempts = 0;
        std::string error;
    };
    void workerLoop();
    void load();
    void saveLocked() const;
    std::string trackPath(const std::string& id) const;
    bool referencedLocked(const std::string& trackId) const;
    void bumpLocked()
    {
        m_revision.fetch_add(1);
    }

    std::string m_dir;
    Hooks m_hooks;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::map<std::string, TrackEntry> m_tracks;
    std::vector<DownloadCollection> m_collections;
    std::vector<std::string> m_deleteQueue; // files to remove, handled by the worker
    std::string m_activeTrack;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_cancelCurrent{false};
    std::atomic<std::uint64_t> m_revision{1};
    std::thread m_thread;
};

} // namespace music
} // namespace miyoofin

#endif
