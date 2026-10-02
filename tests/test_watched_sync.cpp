#include "test_support.hpp"

#include "../src/net/WatchedSync.hpp"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace miyoofin;

namespace {

struct Rig
{
    std::string dir, path;
    std::mutex mutex;
    std::vector<std::string> sent; // "id:1" / "id:0"
    std::atomic<int> failures{0};  // next N sends report Retry
    std::atomic<bool> gone{false}; // sends report Drop
    std::unique_ptr<WatchedSync> sync;
    Session session;
    Rig()
    {
        char tmpl[] = "/tmp/miyoofin-watched-XXXXXX";
        dir = mkdtemp(tmpl);
        path = dir + "/pending.txt";
        session.serverUrl = "http://x";
        session.accessToken = "t";
        session.userId = "u";
        start();
    }
    void start()
    {
        sync = std::make_unique<WatchedSync>(
            path, [this](const Session&, const std::string& id, bool played) {
                if (failures > 0) {
                    --failures;
                    return WatchedSync::Result::Retry;
                }
                if (gone)
                    return WatchedSync::Result::Drop;
                std::lock_guard<std::mutex> lock(mutex);
                sent.push_back(id + ":" + (played ? "1" : "0"));
                return WatchedSync::Result::Done;
            });
        sync->retrySeconds = [] { return 0; };
        sync->setSession(session);
    }
    ~Rig()
    {
        sync.reset();
        std::system(("rm -rf " + dir).c_str());
    }
    template <typename Pred> bool until(Pred pred, int ms = 4000)
    {
        for (int waited = 0; waited < ms; waited += 10) {
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return pred();
    }
    std::vector<std::string> sentCopy()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return sent;
    }
};

void testSendsAndForgets()
{
    std::printf("[test] watched sync sends and forgets\n");
    Rig rig;
    rig.sync->enqueue("m1", true);
    rig.sync->enqueue("e1", false);
    CHECK(rig.until([&] { return rig.sync->pending() == 0 && rig.sentCopy().size() == 2; }));
    CHECK(rig.sentCopy() == (std::vector<std::string>{"m1:1", "e1:0"}));
    std::printf("[test] watched sync sends and forgets OK\n");
}

void testOfflineKeepsAndRetries()
{
    std::printf("[test] watched sync keeps changes while offline\n");
    Rig rig;
    rig.failures = 3;
    rig.sync->enqueue("m1", true);
    CHECK(rig.until([&] { return rig.sentCopy().size() == 1; }));
    CHECK(rig.sync->pending() == 0 || rig.until([&] { return rig.sync->pending() == 0; }));

    // Not signed in: nothing is sent, nothing is lost; signing in sends it.
    Rig quiet;
    quiet.sync->setSession(Session{});
    quiet.sync->enqueue("m2", true);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(quiet.sentCopy().empty() && quiet.sync->pending() == 1);
    quiet.sync->setSession(quiet.session);
    CHECK(quiet.until([&] { return quiet.sentCopy().size() == 1; }));

    // The latest choice for an item wins while it waits.
    Rig latest;
    latest.sync->setSession(Session{});
    latest.sync->enqueue("m3", true);
    latest.sync->enqueue("m3", false);
    CHECK(latest.sync->pending() == 1);
    latest.sync->setSession(latest.session);
    CHECK(latest.until([&] { return latest.sentCopy().size() == 1; }));
    CHECK(latest.sentCopy()[0] == "m3:0");
    std::printf("[test] watched sync keeps changes while offline OK\n");
}

void testSurvivesRestartAndDrops()
{
    std::printf("[test] watched sync survives a restart and drops hopeless changes\n");
    Rig rig;
    rig.sync->setSession(Session{});
    rig.sync->enqueue("m1", true);
    rig.sync.reset(); // the app quits with the change still waiting
    rig.start();
    CHECK(rig.until([&] { return rig.sentCopy().size() == 1 && rig.sync->pending() == 0; }));
    rig.gone = true; // the item no longer exists on the server
    rig.sync->enqueue("deleted", true);
    CHECK(rig.until([&] { return rig.sync->pending() == 0; }));
    CHECK(rig.sentCopy().size() == 1);
    std::printf("[test] watched sync survives a restart and drops hopeless changes OK\n");
}

} // namespace

int main()
{
    testSendsAndForgets();
    testOfflineKeepsAndRetries();
    testSurvivesRestartAndDrops();
    return miyoofin_test::finish("watched_sync");
}
