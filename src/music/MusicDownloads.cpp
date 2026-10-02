#include "MusicDownloads.hpp"
#include "MusicTracks.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {
namespace music {

namespace {

constexpr const char* kMagic = "MFMD=1";

std::string clean(const std::string& s)
{
    std::string out = s;
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r' || c == ',')
            c = ' ';
    return out;
}

std::vector<std::string> split(const std::string& line, char sep)
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = line.find(sep, start);
        if (at == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, at - start));
        start = at + 1;
    }
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

std::string safeId(const std::string& id)
{
    std::string out;
    for (char c : id)
        out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_')
                   ? c
                   : '_';
    return out;
}

} // namespace

MusicDownloads::MusicDownloads(std::string dir, Hooks hooks)
    : m_dir(std::move(dir)), m_hooks(std::move(hooks))
{
    makeDirs(m_dir + "/tracks");
    load();
    m_thread = std::thread([this] { workerLoop(); });
}

MusicDownloads::~MusicDownloads()
{
    m_stop.store(true);
    m_cancelCurrent.store(true);
    m_wake.notify_all();
    m_thread.join();
}

std::string MusicDownloads::trackPath(const std::string& id) const
{
    return m_dir + "/tracks/" + safeId(id) + ".mp3";
}

// ---- persistence
// ----------------------------------------------------------------------------------

void MusicDownloads::saveLocked() const
{
    const std::string path = m_dir + "/index.tsv", tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << kMagic << '\n';
        for (const auto& entry : m_tracks) {
            const Track& t = entry.second.track;
            out << "T\t" << clean(t.id) << '\t' << clean(t.title) << '\t' << clean(t.album) << '\t'
                << clean(t.albumId) << '\t' << clean(t.artist) << '\t' << clean(t.artistId) << '\t'
                << clean(t.albumArtist) << '\t' << clean(t.imageTag) << '\t'
                << clean(t.albumImageTag) << '\t' << t.trackNumber << '\t' << t.discNumber << '\t'
                << t.runTimeTicks << '\t' << entry.second.bytes << '\t'
                << (entry.second.done ? 1 : 0) << '\n';
        }
        for (const DownloadCollection& c : m_collections) {
            out << "C\t" << clean(c.id) << '\t' << clean(c.kind) << '\t' << clean(c.name) << '\t'
                << clean(c.artist) << '\t' << clean(c.artId) << '\t' << clean(c.artTag) << '\t';
            for (std::size_t i = 0; i < c.trackIds.size(); ++i)
                out << (i ? "," : "") << clean(c.trackIds[i]);
            out << '\n';
        }
        if (!out.good())
            return;
    }
    std::rename(tmp.c_str(), path.c_str());
}

void MusicDownloads::load()
{
    std::ifstream in(m_dir + "/index.tsv");
    std::string line;
    if (!std::getline(in, line) || line != kMagic)
        return;
    while (std::getline(in, line)) {
        const auto f = split(line, '\t');
        if (f[0] == "T" && f.size() >= 15) {
            TrackEntry e;
            e.track.id = f[1];
            e.track.title = f[2];
            e.track.album = f[3];
            e.track.albumId = f[4];
            e.track.artist = f[5];
            e.track.artistId = f[6];
            e.track.albumArtist = f[7];
            e.track.imageTag = f[8];
            e.track.albumImageTag = f[9];
            e.track.trackNumber = std::atoi(f[10].c_str());
            e.track.discNumber = std::atoi(f[11].c_str());
            e.track.runTimeTicks = std::strtoll(f[12].c_str(), nullptr, 10);
            e.bytes = std::strtoull(f[13].c_str(), nullptr, 10);
            // Trust the disk over the index: a deleted file is a track to fetch again.
            struct stat st;
            e.done =
                f[14] == "1" && stat(trackPath(e.track.id).c_str(), &st) == 0 && st.st_size > 0;
            if (!e.track.id.empty())
                m_tracks[e.track.id] = std::move(e);
        } else if (f[0] == "C" && f.size() >= 8 && !f[1].empty()) {
            DownloadCollection c;
            c.id = f[1];
            c.kind = f[2];
            c.name = f[3];
            c.artist = f[4];
            c.artId = f[5];
            c.artTag = f[6];
            if (!f[7].empty())
                c.trackIds = split(f[7], ',');
            m_collections.push_back(std::move(c));
        }
    }
}

// ---- queries
// --------------------------------------------------------------------------------------

std::string MusicDownloads::pathFor(const std::string& trackId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tracks.find(trackId);
    return it != m_tracks.end() && it->second.done ? trackPath(trackId) : std::string();
}

bool MusicDownloads::hasTrack(const std::string& trackId) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tracks.find(trackId);
    return it != m_tracks.end() && it->second.done;
}

bool MusicDownloads::hasCollection(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::any_of(m_collections.begin(), m_collections.end(),
                       [&](const DownloadCollection& c) { return c.id == id; });
}

std::vector<Track> MusicDownloads::tracksOf(const std::string& id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<Track> out;
    for (const DownloadCollection& c : m_collections) {
        if (c.id != id)
            continue;
        for (const std::string& trackId : c.trackIds) {
            auto it = m_tracks.find(trackId);
            if (it != m_tracks.end() && it->second.done)
                out.push_back(it->second.track);
        }
    }
    return out;
}

std::vector<DownloadStatus> MusicDownloads::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<DownloadStatus> out;
    for (const DownloadCollection& c : m_collections) {
        DownloadStatus s;
        s.collection = c;
        s.total = static_cast<int>(c.trackIds.size());
        for (const std::string& id : c.trackIds) {
            auto it = m_tracks.find(id);
            if (it == m_tracks.end())
                continue;
            if (it->second.done) {
                ++s.done;
                s.bytes += it->second.bytes;
            } else if (it->second.attempts >= kMaxAttempts) {
                s.failed = true;
                s.error = it->second.error;
            }
            if (id == m_activeTrack)
                s.active = true;
        }
        out.push_back(std::move(s));
    }
    return out;
}

std::uint64_t MusicDownloads::totalBytes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::uint64_t total = 0;
    for (const auto& entry : m_tracks)
        if (entry.second.done)
            total += entry.second.bytes;
    return total;
}

bool MusicDownloads::idle() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_activeTrack.empty() || !m_deleteQueue.empty())
        return false;
    for (const auto& entry : m_tracks)
        if (!entry.second.done && entry.second.attempts < kMaxAttempts &&
            referencedLocked(entry.first))
            return false;
    return true;
}

bool MusicDownloads::referencedLocked(const std::string& trackId) const
{
    for (const DownloadCollection& c : m_collections)
        if (std::find(c.trackIds.begin(), c.trackIds.end(), trackId) != c.trackIds.end())
            return true;
    return false;
}

// ---- changes
// --------------------------------------------------------------------------------------

void MusicDownloads::enqueue(DownloadCollection collection, const std::vector<Track>& tracks)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        collection.trackIds.clear();
        for (const Track& t : tracks) {
            if (t.id.empty())
                continue;
            collection.trackIds.push_back(t.id);
            auto it = m_tracks.find(t.id);
            if (it == m_tracks.end()) {
                TrackEntry e;
                e.track = t;
                m_tracks[t.id] = std::move(e);
            } else if (!it->second.done) {
                it->second.attempts = 0; // asking again clears an earlier failure
                it->second.error.clear();
            }
        }
        auto existing =
            std::find_if(m_collections.begin(), m_collections.end(),
                         [&](const DownloadCollection& c) { return c.id == collection.id; });
        if (existing != m_collections.end())
            *existing = collection;
        else
            m_collections.push_back(collection);
        saveLocked();
        bumpLocked();
    }
    m_wake.notify_all();
}

void MusicDownloads::removeCollection(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = std::find_if(m_collections.begin(), m_collections.end(),
                               [&](const DownloadCollection& c) { return c.id == id; });
        if (it == m_collections.end())
            return;
        const std::vector<std::string> ids = it->trackIds;
        m_collections.erase(it);
        for (const std::string& trackId : ids) {
            if (referencedLocked(trackId))
                continue; // another album or playlist still needs it
            if (trackId == m_activeTrack)
                m_cancelCurrent.store(true);
            m_deleteQueue.push_back(trackId);
        }
        saveLocked();
        bumpLocked();
    }
    m_wake.notify_all();
}

void MusicDownloads::retry(const std::string& id)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const DownloadCollection& c : m_collections) {
            if (c.id != id)
                continue;
            for (const std::string& trackId : c.trackIds) {
                auto it = m_tracks.find(trackId);
                if (it != m_tracks.end() && !it->second.done) {
                    it->second.attempts = 0;
                    it->second.error.clear();
                }
            }
        }
        bumpLocked();
    }
    m_wake.notify_all();
}

// ---- worker
// ---------------------------------------------------------------------------------------

void MusicDownloads::workerLoop()
{
    for (;;) {
        std::string trackId;
        Track track;
        int attempt = 0;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            for (;;) {
                if (m_stop.load())
                    return;
                if (!m_deleteQueue.empty()) {
                    const std::string doomed = m_deleteQueue.back();
                    m_deleteQueue.pop_back();
                    auto it = m_tracks.find(doomed);
                    if (it != m_tracks.end() && !referencedLocked(doomed)) {
                        m_tracks.erase(it);
                        lock.unlock();
                        std::remove(trackPath(doomed).c_str());
                        std::remove((trackPath(doomed) + ".part").c_str());
                        lock.lock();
                        saveLocked();
                        bumpLocked();
                    }
                    continue;
                }
                const bool offline = m_hooks.offline && m_hooks.offline();
                if (!offline) {
                    // First track of the oldest collection that still needs fetching.
                    for (const DownloadCollection& c : m_collections) {
                        for (const std::string& id : c.trackIds) {
                            auto it = m_tracks.find(id);
                            if (it != m_tracks.end() && !it->second.done &&
                                it->second.attempts < kMaxAttempts) {
                                trackId = id;
                                break;
                            }
                        }
                        if (!trackId.empty())
                            break;
                    }
                }
                if (!trackId.empty()) {
                    TrackEntry& e = m_tracks[trackId];
                    track = e.track;
                    attempt = ++e.attempts;
                    m_activeTrack = trackId;
                    m_cancelCurrent.store(false);
                    bumpLocked();
                    break;
                }
                m_wake.wait_for(lock, std::chrono::seconds(offline ? 5 : 30));
            }
        }

        std::string error;
        bool ok = false;
        const std::string part = trackPath(trackId) + ".part";
        const std::uint64_t free = m_hooks.freeBytes ? m_hooks.freeBytes() : 0;
        if (free != 0 && free < kReserveBytes) {
            error = "Not enough space";
            attempt = kMaxAttempts; // retrying will not make room
        } else if (m_hooks.fetch && m_hooks.fetch(trackId, part, error, m_cancelCurrent)) {
            ok = looksLikeAudioFile(part, error) &&
                 std::rename(part.c_str(), trackPath(trackId).c_str()) == 0;
            if (!ok && error.empty())
                error = "Can't store the track";
        } else if (error.empty()) {
            error = "Download failed";
        }
        if (!ok)
            std::remove(part.c_str());

        std::uint64_t bytes = 0;
        if (ok) {
            struct stat st;
            if (stat(trackPath(trackId).c_str(), &st) == 0)
                bytes = static_cast<std::uint64_t>(st.st_size);
        }
        bool backoff = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activeTrack.clear();
            auto it = m_tracks.find(trackId);
            if (it != m_tracks.end()) {
                if (ok) {
                    it->second.done = true;
                    it->second.bytes = bytes;
                    it->second.error.clear();
                } else {
                    it->second.attempts = std::max(it->second.attempts, attempt);
                    it->second.error = error;
                    backoff = it->second.attempts < kMaxAttempts && !m_stop.load();
                }
            } else if (ok) {
                std::remove(trackPath(trackId).c_str()); // removed while it downloaded
            }
            saveLocked();
            bumpLocked();
        }
        if (backoff) {
            // Wait before the next try, but wake for shutdown.
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::seconds(backoffSeconds(attempt)),
                            [this] { return m_stop.load(); });
        }
    }
}

} // namespace music
} // namespace miyoofin
