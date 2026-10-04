#include "DownloadStore.hpp"
#include "MpegTsValidator.hpp"
#include "../cache/LibraryCache.hpp"
#include <atomic>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <set>

namespace miyoofin {

/// Removes the subtitle sidecar files saved beside a download.
void removeSubtitleSidecars(const std::string& itemDir)
{
    std::remove((itemDir + "/playback-tracks.txt").c_str());
    std::remove((itemDir + "/playback-tracks.txt.tmp").c_str());
    const std::string dir = itemDir + "/subs";
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name != "." && name != "..")
                std::remove((dir + "/" + name).c_str());
        }
        closedir(d);
    }
    ::rmdir(dir.c_str());
}
namespace {
bool safeId(const std::string& s)
{
    if (s.empty() || s.size() > 255)
        return false;
    for (unsigned char c : s)
        if (!(std::isalnum(c) || c == '-' || c == '_' || c == '.'))
            return false;
    return true;
}
bool mkdirs(const std::string& p)
{
    for (size_t i = 1; i <= p.size(); ++i)
        if (i == p.size() || p[i] == '/') {
            std::string d = p.substr(0, i);
            if (!d.empty() && ::mkdir(d.c_str(), 0755) && errno != EEXIST)
                return false;
        }
    return true;
}
bool fileSize(const std::string& p, std::uint64_t& out)
{
    struct stat st
    {};
    if (::stat(p.c_str(), &st) || st.st_size < 0)
        return false;
    out = (std::uint64_t)st.st_size;
    return true;
}
std::string enc(const std::string& s)
{
    static const char* x = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_')
            o += c;
        else {
            o += '%';
            o += x[c >> 4];
            o += x[c & 15];
        }
    }
    return o;
}
int hx(char c)
{
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                  : -1;
}
std::string dec(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '%' && i + 2 < s.size() && hx(s[i + 1]) >= 0 && hx(s[i + 2]) >= 0) {
            o += (char)(hx(s[i + 1]) * 16 + hx(s[i + 2]));
            i += 2;
        } else
            o += s[i];
    return o;
}
// Stale temps (manifest.v2.tmp.<pid>.<n>, index.v1.tmp.<pid>.<n>, plus the
// legacy manifest.v2.tmp / index.v1.tmp naming) are left behind only by a
// crash between temp fsync and rename.  Temps are unique per call and every
// manifest/index write runs under DownloadManager::m_mutex, so no live
// writer can own one of these paths when the holder of that same mutex
// sweeps.  Only *.tmp* names are removed: manifest.v2, index.v1, segments
// (*.bin/*.part) and chunks (chunk-*.bin/*.part) never match.
void sweepTmpFiles(const std::string& dir)
{
    DIR* d = opendir(dir.c_str());
    if (!d)
        return;
    dirent* x;
    while ((x = readdir(d))) {
        std::string n = x->d_name;
        if (n.find(".tmp.") != std::string::npos ||
            (n.size() >= 4 && n.rfind(".tmp") == n.size() - 4))
            std::remove((dir + "/" + n).c_str());
    }
    closedir(d);
}
bool atomic(const std::string& p, const std::string& body)
{
    auto q = p.find_last_of('/');
    if (q != std::string::npos && !mkdirs(p.substr(0, q)))
        return false;
    // Unique temp per call (pid + monotonic counter): two writers can never
    // share a temp path, so concurrent whole-file writes cannot interleave,
    // rename over each other mid-flush, or remove each other's temp.  All
    // manifest/index writes here are issued under DownloadManager::m_mutex;
    // this is defence in depth if a future path ever forgets the lock.
    static std::atomic<unsigned long long> s_tmpCounter{0};
    const std::string t = p + ".tmp." + std::to_string((long long)::getpid()) + "." +
                          std::to_string(s_tmpCounter.fetch_add(1));
    FILE* f = std::fopen(t.c_str(), "wb");
    if (!f)
        return false;
    // Close the stream whatever happened (a short-circuited && chain skipped fclose on a write,
    // flush or sync error and leaked a descriptor per failure), keeping each error.
    const bool wrote = std::fwrite(body.data(), 1, body.size(), f) == body.size();
    const bool flushed = std::fflush(f) == 0;
    const bool synced = flushed && ::fsync(fileno(f)) == 0;
    const bool closed = std::fclose(f) == 0;
    const bool ok = wrote && flushed && synced && closed;
    if (!ok || std::rename(t.c_str(), p.c_str())) {
        std::remove(t.c_str());
        return false;
    }
    return true;
}
std::string get(const std::string& b, const char* k)
{
    std::string p = std::string(k) + "=";
    size_t i = b.find(p);
    if (i == std::string::npos || (i && b[i - 1] != '\n'))
        return {};
    i += p.size();
    size_t e = b.find('\n', i);
    return dec(b.substr(i, e == std::string::npos ? std::string::npos : e - i));
}
void put(std::string& b, const char* k, const std::string& v)
{
    b += k;
    b += '=';
    b += enc(v);
    b += '\n';
}
void pu(std::string& b, const char* k, std::uint64_t v)
{
    b += k;
    b += '=';
    b += std::to_string(v);
    b += '\n';
}
bool num(const std::string& s, std::uint64_t& o)
{
    if (s.empty())
        return false;
    char* e = nullptr;
    errno = 0;
    unsigned long long x = std::strtoull(s.c_str(), &e, 10);
    if (errno || *e)
        return false;
    o = x;
    return true;
}
std::string serialize(const DownloadItem& i)
{
    std::string b = i.hlsStorage ? "MFDM=2\n" : "MFDM=1\n";
    put(b, "item", i.itemId);
    put(b, "type", i.itemType);
    put(b, "title", i.title);
    put(b, "series", i.seriesId);
    put(b, "series_name", i.seriesName);
    put(b, "season", i.seasonId);
    put(b, "season_name", i.seasonName);
    put(b, "source", i.mediaSourceId);
    put(b, "available_source", i.availableMediaSourceId);
    put(b, "container", i.container);
    put(b, "etag", i.sourceEtag);
    put(b, "available_etag", i.availableSourceEtag);
    put(b, "video", i.videoCodec);
    put(b, "audio", i.audioCodec);
    put(b, "error", i.lastError);
    put(b, "audio_lang", i.audioLang);
    pu(b, "size", i.expectedSize);
    pu(b, "available_size", i.availableSize);
    pu(b, "chunk", i.chunkSize);
    pu(b, "downloaded", i.downloadedBytes);
    pu(b, "state", (unsigned)i.state);
    pu(b, "runtime", i.runtimeTicks < 0 ? 0 : i.runtimeTicks);
    pu(b, "position", i.playbackPositionTicks < 0 ? 0 : i.playbackPositionTicks);
    pu(b, "season_no", i.seasonNumber);
    pu(b, "episode_no", i.episodeNumber);
    pu(b, "width", i.width);
    pu(b, "height", i.height);
    pu(b, "bitrate", i.bitrate);
    pu(b, "created", i.createdAt);
    pu(b, "updated", i.updatedAt);
    pu(b, "flags",
       (i.localOnly ? 1 : 0) | (i.updateAvailable ? 2 : 0) | (i.externalSubtitles ? 4 : 0));
    if (i.hlsStorage) {
        pu(b, "segments", i.hlsSegmentCount);
        put(b, "profile", i.hlsProfile);
    }
    return b;
}
bool parse(const std::string& b, DownloadItem& i)
{
    bool hls = b.rfind("MFDM=2\n", 0) == 0;
    if (!hls && b.rfind("MFDM=1\n", 0))
        return false;
    i = {};
    i.hlsStorage = hls;
    i.itemId = get(b, "item");
    if (!safeId(i.itemId))
        return false;
    i.itemType = get(b, "type");
    i.title = get(b, "title");
    i.seriesId = get(b, "series");
    i.seriesName = get(b, "series_name");
    i.seasonId = get(b, "season");
    i.seasonName = get(b, "season_name");
    i.mediaSourceId = get(b, "source");
    i.availableMediaSourceId = get(b, "available_source");
    i.container = get(b, "container");
    i.sourceEtag = get(b, "etag");
    i.availableSourceEtag = get(b, "available_etag");
    i.videoCodec = get(b, "video");
    i.audioCodec = get(b, "audio");
    i.lastError = get(b, "error");
    i.audioLang = get(b, "audio_lang");
    std::uint64_t x = 0;
    if (!num(get(b, "size"), i.expectedSize) || !num(get(b, "chunk"), i.chunkSize) ||
        !num(get(b, "downloaded"), i.downloadedBytes) || !num(get(b, "state"), x) ||
        x > (unsigned)DownloadState::NoSpace)
        return false;
    num(get(b, "available_size"), i.availableSize);
    i.state = (DownloadState)x;
    num(get(b, "runtime"), x);
    i.runtimeTicks = x;
    num(get(b, "position"), x);
    i.playbackPositionTicks = x;
    num(get(b, "season_no"), x);
    i.seasonNumber = x;
    num(get(b, "episode_no"), x);
    i.episodeNumber = x;
    num(get(b, "width"), x);
    i.width = x;
    num(get(b, "height"), x);
    i.height = x;
    num(get(b, "bitrate"), i.bitrate);
    num(get(b, "created"), i.createdAt);
    num(get(b, "updated"), i.updatedAt);
    num(get(b, "flags"), x);
    i.localOnly = x & 1;
    i.updateAvailable = x & 2;
    i.externalSubtitles = x & 4;
    if (hls && !num(get(b, "segments"), i.hlsSegmentCount))
        return false;
    i.hlsProfile = get(b, "profile");
    return i.chunkSize > 0;
}
// Like read(), but says why it failed (errno-style) so "not there" and "could not read" differ.
bool readWhy(const std::string& p, std::string& b, int& why)
{
    why = 0;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) {
        why = errno ? errno : EIO;
        return false;
    }
    char q[4096];
    size_t n;
    while ((n = std::fread(q, 1, sizeof q, f)))
        b.append(q, n);
    const bool ok = !std::ferror(f);
    if (!ok)
        why = EIO;
    std::fclose(f);
    return ok;
}
bool read(const std::string& p, std::string& b)
{
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f)
        return false;
    char q[4096];
    size_t n;
    while ((n = std::fread(q, 1, sizeof q, f)))
        b.append(q, n);
    bool ok = !std::ferror(f);
    std::fclose(f);
    return ok;
}
}
std::string DownloadStore::scopeKey(const std::string& a, const std::string& b)
{
    return LibraryCache::scopeKey(a, b);
}
std::string DownloadStore::scopePath(const std::string& s) const
{
    return m_root + "/" + s;
}
std::string DownloadStore::itemPath(const std::string& s, const std::string& i) const
{
    return scopePath(s) + "/items/" + i;
}
std::string DownloadStore::manifestPath(const std::string& s, const std::string& i) const
{
    return itemPath(s, i) + "/manifest.v2";
}
std::string DownloadStore::chunkPath(const std::string& s, const std::string& i, std::uint64_t n,
                                     bool part) const
{
    char b[48];
    std::snprintf(b, sizeof b, "chunk-%06llu.%s", (unsigned long long)n, part ? "part" : "bin");
    return itemPath(s, i) + "/chunks/" + b;
}
std::string DownloadStore::segmentPath(const std::string& s, const std::string& i, std::uint64_t n,
                                       bool part) const
{
    char b[32];
    std::snprintf(b, sizeof b, "%06llu.%s", (unsigned long long)n, part ? "part" : "bin");
    return itemPath(s, i) + "/segments/" + b;
}
bool DownloadStore::ensureHlsDirectories(const std::string& s, const std::string& i) const
{
    return safeId(s) && safeId(i) && storageReadable() && mkdirs(itemPath(s, i) + "/segments");
}
bool DownloadStore::validHlsSegment(const std::string& path, std::uint64_t size, bool full,
                                    std::string* why)
{
    // The pipeline asks the server for H.264 + AAC in MPEG-TS, so that is the one container a
    // segment can legitimately be. See MpegTsValidator.hpp for exactly what a pass guarantees.
    const TsVerdict v = validateMpegTsFile(path, size, full ? TsDepth::Full : TsDepth::Quick);
    if (!v.ok && why)
        *why = v.reason;
    return v.ok;
}

bool DownloadStore::isCompleteSegment(const std::string& s, const std::string& i,
                                      std::uint64_t n) const
{
    const std::string path = segmentPath(s, i, n);
    std::uint64_t z = 0;
    return fileSize(path, z) && validHlsSegment(path, z, false);
}
std::uint64_t DownloadStore::firstIncompleteSegment(const std::string& s,
                                                    const DownloadItem& i) const
{
    for (std::uint64_t n = 0; n < i.hlsSegmentCount; n++)
        if (!isCompleteSegment(s, i.itemId, n))
            return n;
    return i.hlsSegmentCount;
}
std::string DownloadStore::manifestText(const DownloadItem& i)
{
    return serialize(i);
}
bool DownloadStore::saveManifest(const std::string& s, const DownloadItem& i, std::string* e) const
{
    if (!safeId(s) || !safeId(i.itemId) || !storageReadable() ||
        !atomic(manifestPath(s, i.itemId), serialize(i))) {
        if (e)
            *e = "manifest save failed";
        return false;
    }
    noteEstablished();
    return true;
}
bool DownloadStore::loadManifest(const std::string& s, const std::string& id, DownloadItem& i,
                                 std::string* e) const
{
    std::string b;
    if (!safeId(s) || !safeId(id) || !read(manifestPath(s, id), b) || !parse(b, i)) {
        if (e)
            *e = "invalid manifest";
        return false;
    }
    return true;
}
bool DownloadStore::saveIndex(const std::string& s, const std::vector<DownloadItem>& v,
                              std::string* e) const
{
    if (!safeId(s)) {
        if (e)
            *e = "bad scope";
        return false;
    }
    std::string b = "MFDI=1\n";
    std::set<std::string> seen;
    for (const auto& i : v)
        if (seen.insert(i.itemId).second)
            b += i.itemId + "\n";
    if (!storageReadable() || !atomic(scopePath(s) + "/index.v1", b)) {
        if (e)
            *e = "index save failed";
        return false;
    }
    noteEstablished();
    return true;
}
DownloadStore::IndexCreate DownloadStore::createIndexExclusive(const std::string& s,
                                                               const std::vector<DownloadItem>& v,
                                                               std::string* e) const
{
    if (!safeId(s) || !storageReadable() || !mkdirs(scopePath(s))) {
        if (e)
            *e = "index save failed";
        return IndexCreate::Failed;
    }
    std::string b = "MFDI=1\n";
    std::set<std::string> seen;
    for (const auto& i : v)
        if (seen.insert(i.itemId).second)
            b += i.itemId + "\n";
    const int fd = ::open((scopePath(s) + "/index.v1").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        if (errno == EEXIST)
            return IndexCreate::Exists;
        if (e)
            *e = "index save failed";
        return IndexCreate::Failed;
    }
    size_t done = 0;
    bool ok = true;
    while (done < b.size() && ok) {
        const ssize_t n = ::write(fd, b.data() + done, b.size() - done);
        if (n <= 0)
            ok = false;
        else
            done += static_cast<size_t>(n);
    }
    ok = ok && ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok) {
        if (e)
            *e = "index save failed";
        return IndexCreate::Failed; // (a partial file is a damaged index: rebuilt from manifests)
    }
    noteEstablished();
    return IndexCreate::Created;
}
bool DownloadStore::loadIndex(const std::string& s, std::vector<DownloadItem>& v,
                              std::string* e) const
{
    std::string b;
    if (!read(scopePath(s) + "/index.v1", b) || b.rfind("MFDI=1\n", 0)) {
        if (e)
            *e = "invalid index";
        return false;
    }
    std::vector<DownloadItem> loaded;
    std::set<std::string> seen;
    size_t p = 7;
    while (p < b.size()) {
        size_t z = b.find('\n', p);
        std::string id = b.substr(p, z - p);
        if (!id.empty() && seen.insert(id).second) {
            DownloadItem i;
            if (!loadManifest(s, id, i, e))
                return false;
            loaded.push_back(std::move(i));
        }
        if (z == std::string::npos)
            break;
        p = z + 1;
    }
    v.insert(v.end(), loaded.begin(), loaded.end());
    return true;
}
bool DownloadStore::loadCompleteMetadata(const std::string& s, std::vector<DownloadItem>& v,
                                         std::string* e) const
{
    std::vector<DownloadItem> loaded;
    if (!loadIndex(s, loaded, e))
        return false;
    for (const auto& i : loaded)
        if (i.state == DownloadState::Complete || i.state == DownloadState::LocalOnly ||
            i.state == DownloadState::UpdateAvailable)
            v.push_back(i);
    return true;
}
std::string DownloadStore::markerPath() const
{
    std::string root = m_root;
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    const std::size_t slash = root.find_last_of('/');
    const std::string parent =
        slash == std::string::npos ? "." : (slash == 0 ? "/" : root.substr(0, slash));
    const std::string base = slash == std::string::npos ? root : root.substr(slash + 1);
    return parent + "/." + base + ".root";
}
bool DownloadStore::storageReadable() const
{
    struct stat st
    {};
    std::string marker;
    int markerErr = 0;
    const bool haveMarker = readWhy(markerPath(), marker, markerErr);
    if (::stat(m_root.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode) || ::access(m_root.c_str(), R_OK | X_OK) != 0)
            return false;
        if (haveMarker) {
            // An empty mount point left behind by storage that went away is on another device
            // than the library was.
            const std::size_t at = marker.find("root_dev=");
            if (at != std::string::npos && std::strtoull(marker.c_str() + at + 9, nullptr, 10) !=
                                               static_cast<unsigned long long>(st.st_dev))
                return false;
        }
        return true;
    }
    if (errno != ENOENT)
        return false;
    // A dangling link is storage that is not there, not a fresh install.
    struct stat link
    {};
    if (::lstat(m_root.c_str(), &link) == 0)
        return false;
    // Not created yet is fine only for storage that never held a library: the marker beside the
    // root (written after the first successful write) says otherwise. And the place it would be
    // created in must be there; a vanished card takes the parent with it.
    if (haveMarker || markerErr != ENOENT)
        return false;
    std::string parent = m_root;
    while (parent.size() > 1 && parent.back() == '/')
        parent.pop_back();
    const std::size_t slash = parent.find_last_of('/');
    parent = slash == std::string::npos ? "." : (slash == 0 ? "/" : parent.substr(0, slash));
    return ::stat(parent.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
void DownloadStore::noteEstablished() const
{
    if (m_established->load())
        return;
    struct stat st
    {};
    if (::stat(m_root.c_str(), &st) != 0)
        return;
    const std::string body =
        "MFRT=1\nroot_dev=" + std::to_string((unsigned long long)st.st_dev) + "\n";
    if (atomic(markerPath(), body))
        m_established->store(true);
}
LibraryStatus DownloadStore::readLibrary(const std::string& s, std::vector<DownloadItem>& out,
                                         std::string* e) const
{
    auto unavailable = [&](const char* why) {
        if (e)
            *e = why;
        return LibraryStatus::Unavailable;
    };
    if (!safeId(s))
        return unavailable("bad scope");
    if (!storageReadable())
        return unavailable("storage unavailable");
    const std::string scopeDir = scopePath(s);
    struct stat st
    {};
    if (::stat(scopeDir.c_str(), &st) != 0)
        return errno == ENOENT ? LibraryStatus::NewScope : unavailable("scope unreadable");
    if (!S_ISDIR(st.st_mode))
        return unavailable("scope is not a directory");
    // 1. The index and the manifests it names.
    std::string b;
    int why = 0;
    bool needScan = false;
    if (!readWhy(scopeDir + "/index.v1", b, why)) {
        if (why != ENOENT)
            return unavailable("index unreadable");
        needScan = true; // no index: the manifests are the truth
    } else if (b.rfind("MFDI=1\n", 0)) {
        needScan = true; // corrupt index
    }
    std::vector<DownloadItem> loaded;
    if (!needScan) {
        std::set<std::string> seen;
        size_t p = 7;
        while (p < b.size() && !needScan) {
            size_t z = b.find('\n', p);
            std::string id = b.substr(p, z - p);
            if (!id.empty() && seen.insert(id).second) {
                std::string mb;
                DownloadItem i;
                if (!safeId(id)) {
                    needScan = true;
                } else if (!readWhy(manifestPath(s, id), mb, why)) {
                    if (why != ENOENT)
                        return unavailable("manifest unreadable");
                    needScan = true;
                } else if (!parse(mb, i)) {
                    needScan = true;
                } else {
                    loaded.push_back(std::move(i));
                }
            }
            if (z == std::string::npos)
                break;
            p = z + 1;
        }
        if (!needScan) {
            out.insert(out.end(), loaded.begin(), loaded.end());
            return LibraryStatus::Loaded;
        }
    }
    // 2. Reconstruction from the manifests.
    return scanManifests(s, out, e);
}
LibraryStatus DownloadStore::scanManifests(const std::string& s, std::vector<DownloadItem>& out,
                                           std::string* e) const
{
    auto unavailable = [&](const char* why) {
        if (e)
            *e = why;
        return LibraryStatus::Unavailable;
    };
    int why = 0;
    // Same trust as readLibrary: a missing/mismatched storage root or scope is "could not look",
    // never an empty library.
    struct stat st
    {};
    if (!safeId(s) || !storageReadable())
        return unavailable("storage unavailable");
    if (::stat(scopePath(s).c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
        return unavailable("scope unreadable");
    // Every directory must be read; an I/O error anywhere fails the whole scan (a partial scan
    // would look like a smaller library).
    DIR* d = opendir((scopePath(s) + "/items").c_str());
    if (!d)
        return errno == ENOENT ? LibraryStatus::NewScope : unavailable("items unreadable");
    std::vector<DownloadItem> rebuilt;
    std::set<std::string> seen;
    dirent* x;
    errno = 0;
    while ((x = readdir(d))) {
        if (x->d_name[0] == '.' || !seen.insert(x->d_name).second)
            continue;
        if (!safeId(x->d_name))
            continue;
        std::string mb;
        if (!readWhy(manifestPath(s, x->d_name), mb, why)) {
            if (why != ENOENT) {
                closedir(d);
                return unavailable("manifest unreadable");
            }
            continue; // a directory without a manifest
        }
        DownloadItem i;
        if (parse(mb, i))
            rebuilt.push_back(
                std::move(i)); // (a manifest that does not parse is corrupt, not lost)
        errno = 0;
    }
    const bool scanFailed = errno != 0;
    closedir(d);
    if (scanFailed)
        return unavailable("items unreadable");
    out.insert(out.end(), rebuilt.begin(), rebuilt.end());
    return rebuilt.empty() ? LibraryStatus::NewScope : LibraryStatus::Rebuilt;
}
bool DownloadStore::rebuildIndex(const std::string& s, std::vector<DownloadItem>& v,
                                 std::string* e) const
{
    if (!storageReadable()) {
        if (e)
            *e = "storage unavailable";
        return false;
    }
    sweepTmpFiles(scopePath(s));
    DIR* d = opendir((scopePath(s) + "/items").c_str());
    if (!d) {
        if (e)
            *e = "no items";
        return false;
    }
    std::vector<DownloadItem> rebuilt;
    std::set<std::string> seen;
    dirent* x;
    while ((x = readdir(d))) {
        if (x->d_name[0] == '.' || !seen.insert(x->d_name).second)
            continue;
        DownloadItem i;
        if (loadManifest(s, x->d_name, i, nullptr))
            rebuilt.push_back(std::move(i));
    }
    closedir(d);
    if (!saveIndex(s, rebuilt, e))
        return false;
    v.insert(v.end(), rebuilt.begin(), rebuilt.end());
    return true;
}
bool DownloadStore::validateCompletedDownload(const std::string& s, const DownloadItem& i,
                                              std::string* e) const
{
    if (i.hlsStorage) {
        if (!i.hlsSegmentCount)
            return false;
        for (std::uint64_t k = 0; k < i.hlsSegmentCount; k++)
            if (!isCompleteSegment(s, i.itemId, k)) {
                if (e)
                    *e = "missing or empty segment";
                return false;
            }
        return true;
    }
    if (i.expectedSize == 0 || i.chunkSize == 0)
        return false;
    std::uint64_t total = 0, n = chunkCount(i.expectedSize, i.chunkSize);
    for (std::uint64_t k = 0; k < n; k++) {
        std::uint64_t z;
        if (!fileSize(chunkPath(s, i.itemId, k), z) ||
            z != chunkLength(i.expectedSize, k, i.chunkSize)) {
            if (e)
                *e = "missing or wrong-sized chunk";
            return false;
        }
        total = saturatingAdd(total, z);
    }
    if (total != i.expectedSize) {
        if (e)
            *e = "total mismatch";
        return false;
    }
    return true;
}
bool DownloadStore::reconcile(const std::string& s, DownloadItem& i, std::string* e) const
{
    if (!reconcileInMemory(s, i, e))
        return false;
    return saveManifest(s, i, e);
}

// Recomputes the item's byte count and (Downloading -> Queued, all segments present -> Complete)
// state from the files on disk. Reads only: it never writes, so it can run on a copy with no
// lock held and the caller merges the result.
bool DownloadStore::reconcileInMemory(const std::string& s, DownloadItem& i, std::string* e) const
{
    if (i.hlsStorage) {
        // Count only segments that are really media (the same quick structural check that
        // completeness uses), so a damaged or fake file is neither "downloaded bytes" nor a
        // reason to call the item complete; the download resumes from the first bad segment.
        std::uint64_t got = 0, k = 0;
        for (; k < i.hlsSegmentCount; k++) {
            std::uint64_t z = 0;
            const std::string path = segmentPath(s, i.itemId, k);
            if (!fileSize(path, z) || !z || !validHlsSegment(path, z, false))
                break;
            got = saturatingAdd(got, z);
        }
        i.downloadedBytes = got;
        if (i.hlsSegmentCount > 0 && k == i.hlsSegmentCount) {
            if (i.state != DownloadState::UpdateAvailable && i.state != DownloadState::LocalOnly)
                i.state = DownloadState::Complete;
        } else if (i.state == DownloadState::Downloading)
            i.state = DownloadState::Queued;
        return true;
    }
    std::uint64_t got = 0, n = chunkCount(i.expectedSize, i.chunkSize);
    for (std::uint64_t k = 0; k < n; k++) {
        std::uint64_t z = 0, need = chunkLength(i.expectedSize, k, i.chunkSize);
        if (fileSize(chunkPath(s, i.itemId, k), z)) {
            if (z != need) {
                if (e)
                    *e = "bad completed chunk";
                return false;
            }
            got = saturatingAdd(got, z);
            continue;
        }
        if (fileSize(chunkPath(s, i.itemId, k, true), z)) {
            if (z > need) {
                std::remove(chunkPath(s, i.itemId, k, true).c_str());
                z = 0;
            }
            got = saturatingAdd(got, z);
            break;
        }
        break;
    }
    i.downloadedBytes = got;
    if (validateCompletedDownload(s, i, nullptr)) {
        if (i.state != DownloadState::UpdateAvailable && i.state != DownloadState::LocalOnly)
            i.state = DownloadState::Complete;
    } else if (i.state == DownloadState::Downloading)
        i.state = DownloadState::Queued;
    return true;
}
bool DownloadStore::removePartialBytes(const std::string& s, const std::string& id,
                                       std::string* e) const
{
    if (!safeId(s) || !safeId(id)) {
        if (e)
            *e = "unsafe id";
        return false;
    }
    if (!storageReadable()) {
        if (e)
            *e = "storage unavailable";
        return false;
    }
    bool clean = true;
    for (const char* dir : {"chunks", "segments"}) {
        DIR* d = opendir((itemPath(s, id) + "/" + dir).c_str());
        if (!d) {
            if (errno != ENOENT)
                clean = false;
            continue;
        }
        dirent* x;
        while ((x = readdir(d))) {
            std::string n = x->d_name;
            if ((std::string(dir) == "chunks" ? n.rfind("chunk-", 0) == 0 : true) &&
                (n.size() > 4 &&
                 (n.rfind(".bin") == n.size() - 4 || n.rfind(".part") == n.size() - 5)))
                if (std::remove((itemPath(s, id) + "/" + dir + "/" + n).c_str()) != 0 &&
                    errno != ENOENT)
                    clean = false;
        }
        closedir(d);
    }
    if (!clean && e)
        *e = "could not clear old bytes";
    return clean;
}
bool DownloadStore::removeItem(const std::string& s, const std::string& id, std::string* e) const
{
    if (!safeId(s) || !safeId(id)) {
        if (e)
            *e = "unsafe id";
        return false;
    }
    // Before the first deletion: on unavailable or mismatched storage nothing may be touched (the
    // same ids on another volume are not ours to delete). The removal stays owed.
    if (!storageReadable()) {
        if (e)
            *e = "storage unavailable";
        return false;
    }
    DownloadItem i;
    loadManifest(s, id, i, nullptr);
    for (std::uint64_t k = 0; k < chunkCount(i.expectedSize, i.chunkSize); ++k) {
        std::remove(chunkPath(s, id, k).c_str());
        std::remove(chunkPath(s, id, k, true).c_str());
    }
    for (std::uint64_t k = 0; k < i.hlsSegmentCount; k++) {
        std::remove(segmentPath(s, id, k).c_str());
        std::remove(segmentPath(s, id, k, true).c_str());
    }
    std::remove(manifestPath(s, id).c_str());
    removeSubtitleSidecars(itemPath(s, id));
    sweepTmpFiles(itemPath(s, id));
    ::rmdir((itemPath(s, id) + "/chunks").c_str());
    ::rmdir((itemPath(s, id) + "/segments").c_str());
    ::rmdir(itemPath(s, id).c_str());
    // Done only if the manifest is really gone from storage we can read: a failed removal (or
    // unreadable storage) must not be reported as success, or the item returns at the next load.
    struct stat gone
    {};
    if (!storageReadable() || (::stat(manifestPath(s, id).c_str(), &gone) == 0) ||
        errno != ENOENT) {
        if (e)
            *e = "could not remove download";
        return false;
    }
    return true;
}
}
