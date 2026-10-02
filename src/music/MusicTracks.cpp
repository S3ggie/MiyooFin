#include "MusicTracks.hpp"
#include "../net/Session.hpp"
#include <algorithm>
#include <ctime>
#include <cerrno>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#include <vector>

namespace miyoofin {
namespace music {

namespace {

bool fileSize(const std::string& path, std::uint64_t& size)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return false;
    size = static_cast<std::uint64_t>(st.st_size);
    return true;
}

bool makeDirs(const std::string& path)
{
    std::string partial;
    for (std::size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!partial.empty() && ::mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
                return false;
        }
        if (i < path.size())
            partial += path[i];
    }
    return true;
}

} // namespace

bool looksLikeAudioFile(const std::string& path, std::string& error)
{
    std::uint64_t size = 0;
    if (!fileSize(path, size) || size < 1024) {
        error = "Track is empty";
        return false;
    }
    unsigned char head[4] = {0, 0, 0, 0};
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "Track unreadable";
        return false;
    }
    const std::size_t n = std::fread(head, 1, sizeof(head), f);
    std::fclose(f);
    const bool id3 = n >= 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3';
    const bool sync = n >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0;
    if (!id3 && !sync) {
        error = "Server sent something that is not audio";
        return false;
    }
    return true;
}

void pruneCache(const std::string& dir, std::uint64_t limitBytes, const std::string& keep)
{
    struct Entry
    {
        std::string path;
        std::uint64_t size;
        time_t mtime;
    };
    std::vector<Entry> entries;
    std::uint64_t total = 0;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string path = dir + "/" + name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                continue;
            entries.push_back({path, static_cast<std::uint64_t>(st.st_size), st.st_mtime});
            total += static_cast<std::uint64_t>(st.st_size);
        }
        closedir(d);
    }
    std::sort(entries.begin(), entries.end(),
              [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
    for (const Entry& e : entries) {
        if (total <= limitBytes)
            break;
        if (e.path == keep)
            continue;
        if (unlink(e.path.c_str()) == 0)
            total -= e.size;
    }
}

void clearCacheDir(const std::string& dir, const std::set<std::string>& keep)
{
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..")
                continue;
            const std::string path = dir + "/" + name;
            const bool partial = name.size() > 5 && name.compare(name.size() - 5, 5, ".part") == 0;
            struct stat st;
            if (!partial && !keep.count(path) && stat(path.c_str(), &st) == 0 &&
                S_ISREG(st.st_mode))
                unlink(path.c_str());
        }
        closedir(d);
    }
}

ResolvedTrack resolveTrack(const TrackSourceConfig& config, const Track& track,
                           const std::atomic<bool>& cancelled)
{
    ResolvedTrack out;
    if (config.downloadedPath) {
        const std::string local = config.downloadedPath(track.id);
        std::uint64_t size = 0;
        if (!local.empty() && fileSize(local, size) && size > 0) {
            out.ok = out.local = true;
            out.path = local;
            return out;
        }
    }
    const std::string base = config.cacheDir + "/" + track.id + "-" +
                             std::to_string(config.quality.kbps) + "." +
                             trackExtension(config.quality);
    std::string error;
    std::uint64_t size = 0;
    if (fileSize(base, size) && looksLikeAudioFile(base, error)) {
        utime(base.c_str(), nullptr); // most recently used
        out.ok = true;
        out.path = base;
        return out;
    }
    if (config.offline && config.offline()) {
        out.error = "Not available offline";
        return out;
    }
    if (!makeDirs(config.cacheDir)) {
        out.error = "Can't write the music cache";
        return out;
    }
    const std::string part = base + ".part";
    if (!config.fetch || !config.fetch(track.id, part, error, cancelled)) {
        unlink(part.c_str());
        out.error = cancelled.load() ? "Cancelled" : (error.empty() ? "Download failed" : error);
        return out;
    }
    if (!looksLikeAudioFile(part, error)) {
        unlink(part.c_str());
        out.error = error;
        return out;
    }
    if (std::rename(part.c_str(), base.c_str()) != 0) {
        unlink(part.c_str());
        out.error = "Can't store the track";
        return out;
    }
    pruneCache(config.cacheDir, config.cacheLimitBytes, base);
    out.ok = true;
    out.path = base;
    return out;
}

TrackSourceConfig makeServerSource(const Session& session, std::string cacheDir,
                                   AudioQuality quality)
{
    TrackSourceConfig c;
    c.cacheDir = std::move(cacheDir);
    c.quality = quality;
    c.offline = [session] { return session.manualOfflineMode; };
    c.fetch = [session, quality](const std::string& trackId, const std::string& dest,
                                 std::string& error, const std::atomic<bool>& cancelled) {
        return onRoute(session, error, [&](const Connection& conn) {
            return downloadTrack(conn, trackId, quality, dest, error, &cancelled);
        });
    };
    return c;
}

} // namespace music
} // namespace miyoofin
