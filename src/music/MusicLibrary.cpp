#include "MusicLibrary.hpp"
#include "MusicTracks.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {
namespace music {

namespace {

constexpr std::size_t kMaxQueuedCovers = 48;
constexpr std::uint64_t kCoverCacheBytes = 32ull * 1024 * 1024;
constexpr int kCachedItems = 200; // rows kept per cached listing

bool readFile(const std::string& path, std::vector<unsigned char>& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    unsigned char buf[8192];
    std::size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return !out.empty();
}

void makeDirs(const std::string& path)
{
    std::string partial;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!partial.empty())
                ::mkdir(partial.c_str(), 0755);
        }
        if (i < path.size())
            partial += path[i];
    }
}

template <typename T> void trimForCache(Page<T>& page)
{
    if (static_cast<int>(page.items.size()) > kCachedItems)
        page.items.resize(kCachedItems);
}

} // namespace

MusicLibrary::MusicLibrary(Session session, std::string cacheRoot, std::string streamDir)
    : m_session(std::move(session)), m_cache(cacheRoot + "/lists"),
      m_coverDir(cacheRoot + "/covers"),
      m_streamDir(streamDir.empty() ? cacheRoot + "/stream" : std::move(streamDir))
{
    m_offline.store(m_session.manualOfflineMode);
    m_listThread = std::thread([this] { listLoop(); });
    m_coverThread = std::thread([this] { coverLoop(); });
}

MusicLibrary::~MusicLibrary()
{
    {
        // Under the mutex, so a worker between its predicate check and its wait cannot miss it.
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true);
        m_cancelCurrentList.store(true);
    }
    m_wake.notify_all();
    m_listThread.join();
    m_coverThread.join();
}

std::string MusicLibrary::coverKey(const std::string& itemId, const std::string& tag, int size)
{
    return itemId + ":" + tag + ":" + std::to_string(size);
}

std::uint64_t MusicLibrary::requestList(const ListingRequest& request)
{
    std::uint64_t ticket;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ticket = m_nextTicket++;
        m_listJobs.push_back({ticket, request, m_generation.load()});
    }
    m_wake.notify_all();
    return ticket;
}

void MusicLibrary::clearCaches(std::set<std::string> keep)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_clearKeep = std::move(keep);
    }
    m_clearRequested.store(true);
    m_wake.notify_all();
}

void MusicLibrary::cancelLists()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_listJobs.clear();
        m_listResults.clear();
        m_generation.fetch_add(1);
    }
    m_cancelCurrentList.store(true);
}

void MusicLibrary::requestCover(const std::string& itemId, const std::string& tag, int size)
{
    if (itemId.empty())
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const CoverJob& j : m_coverJobs)
            if (j.itemId == itemId && j.tag == tag && j.size == size)
                return;
        m_coverJobs.push_back({itemId, tag, size});
        while (m_coverJobs.size() > kMaxQueuedCovers) {
            // The user has scrolled on; the oldest is stale. Say so, so it can be asked again.
            CoverResult dropped;
            dropped.key = coverKey(m_coverJobs.front().itemId, m_coverJobs.front().tag,
                                   m_coverJobs.front().size);
            dropped.dropped = true;
            m_coverResults.push_back(std::move(dropped));
            m_coverJobs.pop_front();
        }
    }
    m_wake.notify_all();
}

std::vector<ListResult> MusicLibrary::takeLists()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ListResult> out;
    out.swap(m_listResults);
    return out;
}

std::vector<CoverResult> MusicLibrary::takeCovers()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CoverResult> out;
    out.swap(m_coverResults);
    return out;
}

void MusicLibrary::runList(const ListJob& job)
{
    const std::string key = job.request.key();
    auto publish = [&](ListResult&& r) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (job.generation == m_generation.load())
            m_listResults.push_back(std::move(r));
    };
    auto makeResult = [&] {
        ListResult r;
        r.ticket = job.ticket;
        r.request = job.request;
        return r;
    };
    const Listing kind = job.request.kind;
    const bool tracksKind = kind == Listing::Songs || kind == Listing::RecentlyPlayed ||
                            kind == Listing::AlbumTracks || kind == Listing::PlaylistTracks;
    const bool albumsKind =
        kind == Listing::Albums || kind == Listing::RecentAlbums || kind == Listing::ArtistAlbums;

    if (job.request.start == 0) { // paint from the cache while the server answers
        ListResult r = makeResult();
        r.fromCache = true;
        r.final = false;
        bool hit = false;
        if (tracksKind)
            hit = m_cache.loadTracks(key, r.tracks.items, r.tracks.total);
        else if (albumsKind)
            hit = m_cache.loadAlbums(key, r.albums.items, r.albums.total);
        else if (kind == Listing::Artists)
            hit = m_cache.loadArtists(key, r.artists.items, r.artists.total);
        else
            hit = m_cache.loadPlaylists(key, r.playlists.items, r.playlists.total);
        if (hit)
            publish(std::move(r));
    }

    ListResult r = makeResult();
    std::string error;
    bool ok = false;
    if (m_offline.load()) {
        error = "Offline";
    } else {
        m_cancelCurrentList.store(false);
        // A cancel (or shutdown) that landed while this job was being picked up must win.
        if (m_stop.load() || job.generation != m_generation.load())
            return;
        ok = onRoute(m_session, error, [&](const Connection& c) {
            const std::atomic<bool>* cancelled = &m_cancelCurrentList;
            if (tracksKind)
                return fetchTracks(c, job.request, r.tracks, error, cancelled);
            if (albumsKind)
                return fetchAlbums(c, job.request, r.albums, error, cancelled);
            if (kind == Listing::Artists)
                return fetchArtists(c, job.request, r.artists, error, cancelled);
            return fetchPlaylists(c, job.request, r.playlists, error, cancelled);
        });
    }
    r.ok = ok;
    r.error = error;
    if (ok && job.request.start == 0) {
        if (tracksKind) {
            Page<Track> copy = r.tracks;
            trimForCache(copy);
            m_cache.saveTracks(key, copy.items, copy.total);
        } else if (albumsKind) {
            Page<Album> copy = r.albums;
            trimForCache(copy);
            m_cache.saveAlbums(key, copy.items, copy.total);
        } else if (kind == Listing::Artists) {
            Page<Artist> copy = r.artists;
            trimForCache(copy);
            m_cache.saveArtists(key, copy.items, copy.total);
        } else {
            Page<Playlist> copy = r.playlists;
            trimForCache(copy);
            m_cache.savePlaylists(key, copy.items, copy.total);
        }
    }
    publish(std::move(r));
}

void MusicLibrary::listLoop()
{
    for (;;) {
        ListJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop.load() || !m_listJobs.empty(); });
            if (m_stop.load())
                return;
            job = m_listJobs.front();
            m_listJobs.pop_front();
        }
        if (job.generation != m_generation.load())
            continue;
        runList(job);
    }
}

void MusicLibrary::coverLoop()
{
    for (;;) {
        CoverJob job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait(lock, [this] {
                return m_stop.load() || !m_coverJobs.empty() || m_clearRequested.load();
            });
            if (m_stop.load())
                return;
            if (m_clearRequested.exchange(false)) {
                const std::set<std::string> keep = m_clearKeep;
                lock.unlock();
                pruneCache(m_coverDir, 0, "");
                pruneCache(m_cache.dir(), 0, "");
                clearCacheDir(m_streamDir, keep); // not the track playing or queued next
                continue;
            }
            job = m_coverJobs.back(); // newest first: what the user is looking at now
            m_coverJobs.pop_back();
        }
        CoverResult result;
        result.key = coverKey(job.itemId, job.tag, job.size);
        const std::string path = m_coverDir + "/" + MusicCache::fileKey(result.key) + ".jpg";
        std::vector<unsigned char> jpeg;
        if (!readFile(path, jpeg)) {
            if (!m_offline.load()) {
                std::string error;
                makeDirs(m_coverDir);
                const bool fetched = onRoute(m_session, error, [&](const Connection& c) {
                    return fetchCover(c, job.itemId, job.tag, job.size, jpeg, error, &m_stop);
                });
                if (fetched) {
                    const std::string tmp = path + ".tmp";
                    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
                        std::fwrite(jpeg.data(), 1, jpeg.size(), f);
                        std::fclose(f);
                        std::rename(tmp.c_str(), path.c_str());
                        pruneCache(m_coverDir, kCoverCacheBytes, path);
                    }
                } else {
                    jpeg.clear();
                }
            }
        }
        if (!jpeg.empty()) {
            result.image = ImageDecoder::decodeJpeg(jpeg.data(), jpeg.size());
            result.ok = !result.image.empty();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_coverResults.push_back(std::move(result));
    }
}

} // namespace music
} // namespace miyoofin
