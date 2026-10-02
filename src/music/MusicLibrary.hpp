#ifndef MIYOOFIN_MUSIC_LIBRARY_HPP
#define MIYOOFIN_MUSIC_LIBRARY_HPP

#include "../image/ImageDecoder.hpp"
#include "../net/Session.hpp"
#include "MusicApi.hpp"
#include "MusicCache.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace miyoofin {
namespace music {

/// One answer to a listing request. A request first answers from the on-disk cache
/// (`fromCache`, never final) and then from the server (`final`); a failed server call
/// ends with `ok=false` and the cached rows stay on screen.
struct ListResult
{
    std::uint64_t ticket = 0;
    ListingRequest request;
    bool fromCache = false, ok = true, final = true;
    std::string error;
    Page<Track> tracks;
    Page<Album> albums;
    Page<Artist> artists;
    Page<Playlist> playlists;
};

struct CoverResult
{
    std::string key; // MusicLibrary::coverKey
    DecodedImage image;
    bool ok = false;
    bool dropped = false; // never fetched (queue overflow): ask again if it is still needed
};

/// Everything the music screens read from the server, off the UI thread: listings (two
/// pages deep in a cache for instant redraws) and cover art (disk cache + decode). The UI
/// thread only calls the cheap request/take methods.
class MusicLibrary
{
  public:
    /// `cacheRoot` holds lists/ and covers/; `streamDir` is where streamed tracks are kept.
    MusicLibrary(Session session, std::string cacheRoot, std::string streamDir = "");
    ~MusicLibrary();
    MusicLibrary(const MusicLibrary&) = delete;
    MusicLibrary& operator=(const MusicLibrary&) = delete;

    /// Queues a listing. Pages after the first (`start > 0`) skip the cache.
    std::uint64_t requestList(const ListingRequest& request);
    /// Drops queued listings and stops the one in flight; their results are discarded.
    void cancelLists();
    void requestCover(const std::string& itemId, const std::string& tag, int size);
    /// Deletes cached listings, covers and streamed tracks (downloads are elsewhere) on a
    /// worker thread.
    void clearCaches();
    void setOffline(bool offline)
    {
        m_offline.store(offline);
    }

    std::vector<ListResult> takeLists();
    std::vector<CoverResult> takeCovers();

    static std::string coverKey(const std::string& itemId, const std::string& tag, int size);
    const std::string& coverDir() const
    {
        return m_coverDir;
    }

  private:
    struct ListJob
    {
        std::uint64_t ticket;
        ListingRequest request;
        std::uint64_t generation;
    };
    struct CoverJob
    {
        std::string itemId, tag;
        int size;
    };
    void listLoop();
    void coverLoop();
    void runList(const ListJob& job);

    Session m_session;
    MusicCache m_cache;
    std::string m_coverDir;
    std::atomic<bool> m_offline{false};
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_generation{1};
    std::atomic<bool> m_cancelCurrentList{false};
    std::atomic<bool> m_clearRequested{false};
    std::string m_streamDir;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<ListJob> m_listJobs;
    std::deque<CoverJob> m_coverJobs;
    std::vector<ListResult> m_listResults;
    std::vector<CoverResult> m_coverResults;
    std::uint64_t m_nextTicket = 1;
    std::thread m_listThread, m_coverThread;
};

} // namespace music
} // namespace miyoofin

#endif
