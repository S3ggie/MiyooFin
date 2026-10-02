#include "test_support.hpp"

#include "../src/music/MusicDownloads.hpp"

#include <chrono>
#include <cstdlib>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

using namespace miyoofin::music;

namespace {

Track track(const std::string& id)
{
    Track t;
    t.id = id;
    t.title = "Song " + id;
    t.album = "Album";
    t.albumId = "al";
    t.artist = "Artist";
    t.runTimeTicks = 1800000000;
    return t;
}

std::vector<Track> tracks(std::initializer_list<const char*> ids)
{
    std::vector<Track> v;
    for (const char* id : ids)
        v.push_back(track(id));
    return v;
}

DownloadCollection collection(const std::string& id, const std::string& kind = "album")
{
    DownloadCollection c;
    c.id = id;
    c.kind = kind;
    c.name = "Collection " + id;
    c.artist = "Artist";
    return c;
}

struct Rig
{
    std::string dir;
    std::atomic<int> fetches{0};
    std::atomic<bool> offline{false};
    std::atomic<int> failFirst{0}; // number of upcoming fetches that fail
    std::atomic<bool> garbage{false};
    std::uint64_t freeBytes = 0;
    std::unique_ptr<MusicDownloads> dl;
    Rig()
    {
        char tmpl[] = "/tmp/miyoofin-music-dl-XXXXXX";
        dir = mkdtemp(tmpl);
        start();
    }
    void start()
    {
        MusicDownloads::Hooks hooks;
        hooks.fetch = [this](const std::string& id, const std::string& dest, std::string& error,
                             const std::atomic<bool>&) {
            ++fetches;
            if (failFirst > 0) {
                --failFirst;
                error = "boom";
                return false;
            }
            FILE* f = std::fopen(dest.c_str(), "wb");
            const std::string body =
                garbage ? std::string(3000, 'x') : "ID3" + std::string(2997, 'a');
            std::fwrite(body.data(), 1, body.size(), f);
            std::fclose(f);
            (void)id;
            return true;
        };
        hooks.offline = [this] { return offline.load(); };
        hooks.freeBytes = [this] { return freeBytes; };
        dl = std::make_unique<MusicDownloads>(dir, hooks);
        dl->backoffSeconds = [](int) { return 0; };
    }
    ~Rig()
    {
        dl.reset();
        std::system(("rm -rf " + dir).c_str());
    }
    template <typename Pred> bool until(Pred pred, int ms = 5000)
    {
        for (int waited = 0; waited < ms; waited += 20) {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    }
};

bool exists(const std::string& path)
{
    return access(path.c_str(), F_OK) == 0;
}

void testDownloadsAlbum()
{
    std::printf("[test] music downloads: an album\n");
    Rig rig;
    CHECK(!rig.dl->hasTrack("a") && rig.dl->pathFor("a").empty());
    rig.dl->enqueue(collection("al1"), tracks({"a", "b", "c"}));
    CHECK(rig.until([&] { return rig.dl->idle() && rig.dl->hasTrack("c"); }));
    CHECK(rig.fetches == 3 && rig.dl->hasTrack("a") && rig.dl->hasTrack("b"));
    CHECK(exists(rig.dl->pathFor("a")) &&
          rig.dl->pathFor("a").find("/tracks/a.mp3") != std::string::npos);
    const auto status = rig.dl->snapshot();
    CHECK(status.size() == 1 && status[0].done == 3 && status[0].total == 3 &&
          status[0].bytes == 9000 && !status[0].failed);
    CHECK(rig.dl->totalBytes() == 9000);
    CHECK(rig.dl->hasCollection("al1"));
    const auto done = rig.dl->tracksOf("al1");
    CHECK(done.size() == 3 && done[0].id == "a" && done[2].title == "Song c");
    std::printf("[test] music downloads: an album OK\n");
}

void testSharedTracksAndRemoval()
{
    std::printf("[test] music downloads: shared tracks and removal\n");
    Rig rig;
    rig.dl->enqueue(collection("al1"), tracks({"a", "b"}));
    rig.dl->enqueue(collection("pl1", "playlist"), tracks({"b", "c"}));
    CHECK(rig.until([&] { return rig.dl->idle() && rig.dl->hasTrack("c"); }));
    CHECK(rig.fetches == 3); // "b" is stored once
    const std::string pathB = rig.dl->pathFor("b"), pathA = rig.dl->pathFor("a");
    const std::uint64_t revision = rig.dl->revision();
    rig.dl->removeCollection("al1");
    CHECK(rig.until([&] { return !rig.dl->hasTrack("a"); }));
    CHECK(!exists(pathA) && exists(pathB)); // the playlist still needs "b"
    CHECK(!rig.dl->hasCollection("al1") && rig.dl->hasCollection("pl1"));
    CHECK(rig.dl->revision() > revision);
    rig.dl->removeCollection("pl1");
    CHECK(rig.until([&] { return !rig.dl->hasTrack("b") && !rig.dl->hasTrack("c"); }));
    CHECK(!exists(pathB) && rig.dl->snapshot().empty() && rig.dl->totalBytes() == 0);
    std::printf("[test] music downloads: shared tracks and removal OK\n");
}

void testPersistence()
{
    std::printf("[test] music downloads: survives a restart\n");
    Rig rig;
    rig.dl->enqueue(collection("al1"), tracks({"a", "b"}));
    CHECK(rig.until([&] { return rig.dl->idle() && rig.dl->hasTrack("b"); }));
    rig.dl.reset(); // app restart
    rig.start();
    CHECK(rig.dl->hasTrack("a") && rig.dl->hasTrack("b") && rig.dl->hasCollection("al1"));
    CHECK(rig.dl->tracksOf("al1").size() == 2);
    // A file deleted behind our back is noticed at startup and fetched again, only that one.
    const int before = rig.fetches;
    std::remove(rig.dl->pathFor("a").c_str());
    rig.dl.reset();
    rig.start();
    CHECK(rig.until([&] { return rig.dl->idle() && rig.dl->hasTrack("a"); }));
    CHECK(rig.dl->hasTrack("b") && rig.fetches == before + 1);
    std::printf("[test] music downloads: survives a restart OK\n");
}

void testRetryAndFailure()
{
    std::printf("[test] music downloads: retries and failure\n");
    Rig rig;
    rig.failFirst = 2; // two failures, then it works (3 attempts allowed)
    rig.dl->enqueue(collection("al1"), tracks({"a"}));
    CHECK(rig.until([&] { return rig.dl->hasTrack("a"); }));
    CHECK(rig.fetches == 3);

    rig.failFirst = 100;
    rig.dl->enqueue(collection("al2"), tracks({"x"}));
    CHECK(rig.until([&] {
        const auto s = rig.dl->snapshot();
        return s.size() == 2 && s[1].failed;
    }));
    CHECK(rig.dl->snapshot()[1].error == "boom" && !rig.dl->hasTrack("x"));
    rig.failFirst = 0;
    rig.dl->retry("al2");
    CHECK(rig.until([&] { return rig.dl->hasTrack("x"); }));
    CHECK(!rig.dl->snapshot()[1].failed);

    // A server answer that is not audio is a failure, not a corrupt "download".
    rig.garbage = true;
    rig.dl->enqueue(collection("al3"), tracks({"g"}));
    CHECK(
        rig.until([&] { return rig.dl->snapshot().size() == 3 && rig.dl->snapshot()[2].failed; }));
    CHECK(!rig.dl->hasTrack("g") && !exists(rig.dir + "/tracks/g.mp3"));
    std::printf("[test] music downloads: retries and failure OK\n");
}

void testOfflineAndSpace()
{
    std::printf("[test] music downloads: offline and low space\n");
    Rig rig;
    rig.offline = true;
    rig.dl->enqueue(collection("al1"), tracks({"a"}));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rig.fetches == 0 && !rig.dl->hasTrack("a")); // waits instead of failing
    rig.offline = false;
    // The worker re-checks every few seconds while offline; a new request wakes it at once.
    rig.dl->enqueue(collection("al1"), tracks({"a"}));
    CHECK(rig.until([&] { return rig.dl->hasTrack("a"); }, 8000));

    rig.freeBytes = 10 * 1024 * 1024; // below the reserve
    rig.dl->enqueue(collection("al2"), tracks({"b"}));
    CHECK(
        rig.until([&] { return rig.dl->snapshot().size() == 2 && rig.dl->snapshot()[1].failed; }));
    CHECK(rig.dl->snapshot()[1].error == "Not enough space" && !rig.dl->hasTrack("b"));
    std::printf("[test] music downloads: offline and low space OK\n");
}

} // namespace

int main()
{
    testDownloadsAlbum();
    testSharedTracksAndRemoval();
    testPersistence();
    testRetryAndFailure();
    testOfflineAndSpace();
    return miyoofin_test::finish("music_downloads");
}
