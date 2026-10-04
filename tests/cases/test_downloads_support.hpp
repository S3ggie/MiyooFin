#ifndef MIYOOFIN_TEST_DOWNLOADS_SUPPORT_HPP
#define MIYOOFIN_TEST_DOWNLOADS_SUPPORT_HPP

// Shared fixtures and helpers for the test_downloads* groups. Included by every group; helpers are
// inline/anonymous so a group that does not use one does not warn about it.

#include "../test_support.hpp"
#include "../../src/download/DownloadSubtitles.hpp"
#include "../../src/download/DownloadAudio.hpp"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <csignal>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <dirent.h>

inline std::string readFixture(const std::string& path)
{
    std::string out;
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file)
        return out;
    char buffer[128];
    std::size_t bytes = 0;
    while ((bytes = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
        out.append(buffer, bytes);
    std::fclose(file);
    return out;
}

#include "../../src/ui/HomeDownloadsState.hpp"
#include <memory>
#include <sys/stat.h>
#include <thread>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "../../src/library/LibraryQuery.hpp"
#include "../../src/library/LibraryCoordinator.hpp"
#include "../../src/net/TlsConfig.hpp"

static inline DownloadItem planItem(const std::string& id, std::uint64_t size,
                                    std::uint64_t done = 0)
{
    DownloadItem i;
    i.itemId = id;
    i.expectedSize = size;
    i.downloadedBytes = done;
    return i;
}

static inline void writeFixture(const std::string& path, const char* data, size_t size)
{
    FILE* f = fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (f) {
        CHECK(fwrite(data, 1, size, f) == size);
        fclose(f);
    }
}
// Real MPEG-TS segments (ffmpeg H.264 + AAC output, the same structure Jellyfin produces):
// A is 38728 bytes, B is 24064.
static const size_t kSegA = 38728, kSegB = 24064;
static inline std::string legitSegment(bool a)
{
    std::ifstream in(a ? "tests/fixtures/hls/legit-2s.ts" : "tests/fixtures/hls/legit-tail.ts",
                     std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static inline void writeLegit(const std::string& path, bool a)
{
    const std::string d = legitSegment(a);
    writeFixture(path, d.data(), d.size());
}
static inline DownloadItem restartFixture(const std::string& id,
                                          DownloadState state = DownloadState::Queued)
{
    DownloadItem item;
    item.itemId = id;
    item.chunkSize = 1;
    item.hlsStorage = true;
    item.hlsSegmentCount = 2;
    item.hlsProfile = "test";
    item.state = state;
    return item;
}

// Abandoned-delete-flag consumption, stale-segment removal under lock, and
// the transfer I/O persist shapes are only observable through a live
// transfer racing the reconciler/erase paths over real network; they cannot
// be reproduced deterministically on the host.  The decision helpers those
// paths call (decideTransferFinish delete-wins, applyTransferValidationFailure,
// publishTransferProgress, applyReconciledSource) are covered behaviourally in
// the neighbouring tests, and the final on-disk persist invariants are covered
// by testWorkerPersistMatchesLiveState.  The former call-site source pins are
// removed rather than pretending a unit test covers the race.

static inline std::string manifestIdentity(DownloadStore& store, const std::string& scope,
                                           const std::string& id)
{
    // Atomic rename always mints a new inode, so an untouched manifest keeps
    // its identity even on filesystems with coarse mtime granularity.
    struct stat st
    {};
    if (::stat(store.manifestPath(scope, id).c_str(), &st))
        return {};
    return std::to_string((unsigned long long)st.st_ino) + ":" +
           std::to_string((long long)st.st_mtime) + ":" + std::to_string((long long)st.st_size);
}

// A write that fails at flush time (here: the file-size limit) must not leak the descriptor.
static inline int openFdCount()
{
    int n = 0;
    if (DIR* d = opendir("/proc/self/fd")) {
        while (readdir(d))
            n++;
        closedir(d);
    }
    return n;
}

// pause()/resume() change memory and return: a stalled disk must not stall the UI thread or the
// snapshot lock, and once it answers the live state is what ends up on disk.
namespace {
// Holds the storage thread inside its next disk write until release() so a test can issue UI
// calls while "the disk is stalled". The hook is removed by the destructor.
struct DiskStall
{
    // Latch state is shared with the hook the manager holds, so it outlives this object for as
    // long as any storage-thread invocation can still be inside the callback; release() also
    // waits for those invocations to leave (a notify alone is not a completion barrier).
    struct State
    {
        std::mutex m;
        std::condition_variable cv;
        bool reached = false, released = false;
    };
    DownloadManager& manager;
    std::shared_ptr<State> st = std::make_shared<State>();
    explicit DiskStall(DownloadManager& mgr) : manager(mgr)
    {
        std::shared_ptr<State> state = st;
        manager.setAsyncWriteHookForTest([state] {
            std::unique_lock<std::mutex> l(state->m);
            state->reached = true;
            state->cv.notify_all();
            state->cv.wait(l, [&] { return state->released; });
        });
    }
    ~DiskStall()
    {
        release();
    }
    bool waitReached()
    {
        std::unique_lock<std::mutex> l(st->m);
        return st->cv.wait_for(l, std::chrono::seconds(4), [this] { return st->reached; });
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> l(st->m);
            st->released = true;
        }
        st->cv.notify_all();
        manager.setAsyncWriteHookForTest(nullptr); // returns once no invocation is running
    }
};
} // namespace

namespace {
inline bool seedItem(DownloadStore& store, const std::string& scope, DownloadItem item,
                     bool segment)
{
    std::vector<DownloadItem> index;
    store.loadIndex(scope, index, nullptr);
    if (!store.ensureHlsDirectories(scope, item.itemId) ||
        !store.saveManifest(scope, item, nullptr))
        return false;
    if (segment)
        for (std::uint64_t k = 0; k < item.hlsSegmentCount; ++k)
            writeLegit(store.segmentPath(scope, item.itemId, k), k == 0);
    index.push_back(item);
    return store.saveIndex(scope, index, nullptr);
}
} // namespace

static inline Session fixtureAccount(const std::string& user)
{
    Session s;
    s.serverUrl = "http://127.0.0.1:9";
    s.userId = user;
    s.accessToken = "fake";
    s.deviceId = "d";
    return s;
}

namespace {
// Minimal loopback HTTP server whose handler may block (a "held" response). One request at a time.
class HeldServer
{
  public:
    explicit HeldServer(std::function<std::string(const std::string&)> handler)
        : m_handler(std::move(handler))
    {
        m_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        ::listen(m_fd, 8);
        socklen_t len = sizeof(addr);
        ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_thread = std::thread([this] { run(); });
    }
    ~HeldServer()
    {
        m_stop = true;
        ::shutdown(m_fd, SHUT_RDWR);
        ::close(m_fd);
        m_thread.join();
    }
    std::string base() const
    {
        return "http://127.0.0.1:" + std::to_string(m_port);
    }
    int hits() const
    {
        return m_hits.load();
    }

  private:
    void run()
    {
        while (!m_stop) {
            const int client = ::accept(m_fd, nullptr, nullptr);
            if (client < 0)
                return;
            char buf[4096];
            const ssize_t n = ::recv(client, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                ++m_hits;
                const std::string reply = m_handler(std::string(buf, buf + n));
                std::size_t sent = 0;
                while (sent < reply.size()) {
                    const ssize_t w =
                        ::send(client, reply.data() + sent, reply.size() - sent, MSG_NOSIGNAL);
                    if (w <= 0)
                        break;
                    sent += static_cast<std::size_t>(w);
                }
            }
            ::close(client);
        }
    }
    std::function<std::string(const std::string&)> m_handler;
    int m_fd = -1;
    unsigned short m_port = 0;
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_hits{0};
    std::thread m_thread;
};

inline std::string httpReply(int code, const std::string& body)
{
    return "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Err") +
           "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
           body;
}

// A held response: the handler parks until released.
struct Gate
{
    std::mutex m;
    std::condition_variable cv;
    bool reached = false, open = false;
    void hold()
    {
        std::unique_lock<std::mutex> l(m);
        reached = true;
        cv.notify_all();
        cv.wait(l, [this] { return open; });
    }
    bool waitReached()
    {
        std::unique_lock<std::mutex> l(m);
        return cv.wait_for(l, std::chrono::seconds(8), [this] { return reached; });
    }
    void release()
    {
        {
            std::lock_guard<std::mutex> l(m);
            open = true;
        }
        cv.notify_all();
    }
};

inline DownloadItem pickItem(DownloadManager& manager, const std::string& id)
{
    for (const auto& i : manager.snapshot().items)
        if (i.itemId == id)
            return i;
    return DownloadItem{};
}
} // namespace

namespace {
// Makes every write under `root` fail: the directory is moved aside and a regular file sits in its
// place (the "SD card went away" shape). unblock() puts the data back.
struct BlockedStorage
{
    std::string root, aside;
    bool blocked = false;
    explicit BlockedStorage(std::string r) : root(std::move(r)), aside(root + ".aside") {}
    ~BlockedStorage()
    {
        unblock();
    }
    // Both directions swap the two paths in one step (RENAME_EXCHANGE): a running manager retries
    // on its own and would recreate a missing root between a separate unlink and rename.
    void block()
    {
        if (blocked)
            return;
        ::mkdir(root.c_str(),
                0755); // (a first run writes nothing until there is something to save)
        FILE* f = std::fopen(aside.c_str(), "w");
        CHECK(f != nullptr);
        if (f)
            std::fclose(f);
        CHECK(::renameat2(AT_FDCWD, root.c_str(), AT_FDCWD, aside.c_str(), RENAME_EXCHANGE) == 0);
        blocked = true;
    }
    void unblock()
    {
        if (!blocked)
            return;
        CHECK(::renameat2(AT_FDCWD, root.c_str(), AT_FDCWD, aside.c_str(), RENAME_EXCHANGE) == 0);
        ::unlink(aside.c_str()); // the stand-in file
        blocked = false;
    }
};

template <class Pred> bool waitUntil(Pred pred, int ms = 6000)
{
    for (int n = 0; n < ms / 10; ++n) {
        if (pred())
            return true;
        usleep(10000);
    }
    return pred();
}

inline bool manifestHas(DownloadStore& store, const std::string& scope, const std::string& id,
                        DownloadState state)
{
    DownloadItem m;
    return store.loadManifest(scope, id, m, nullptr) && m.state == state;
}

inline bool indexLists(DownloadStore& store, const std::string& scope, const std::string& id)
{
    std::vector<DownloadItem> idx;
    if (!store.loadIndex(scope, idx, nullptr))
        return false;
    return std::any_of(idx.begin(), idx.end(),
                       [&](const DownloadItem& i) { return i.itemId == id; });
}
} // namespace

namespace {
inline std::string slurp(const std::string& path)
{
    std::string out;
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char buf[512];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

inline std::set<std::string> liveIds(DownloadManager& manager)
{
    std::set<std::string> ids;
    for (const auto& i : manager.snapshot().items)
        ids.insert(i.itemId);
    return ids;
}

inline std::set<std::string> indexIds(DownloadStore& store, const std::string& scope)
{
    std::set<std::string> ids;
    std::vector<DownloadItem> idx;
    if (store.loadIndex(scope, idx, nullptr))
        for (const auto& i : idx)
            ids.insert(i.itemId);
    return ids;
}
} // namespace

namespace {
// Every regular file under `dir` with its bytes (to prove nothing there was touched).
inline void snapshotTree(const std::string& dir, std::map<std::string, std::string>& out)
{
    DIR* d = ::opendir(dir.c_str());
    if (!d)
        return;
    while (dirent* e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        const std::string path = dir + "/" + name;
        struct stat st
        {};
        if (::stat(path.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            snapshotTree(path, out);
        else
            out[path] = slurp(path);
    }
    ::closedir(d);
}
} // namespace

#endif
