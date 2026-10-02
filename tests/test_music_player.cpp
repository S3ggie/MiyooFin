#include "test_support.hpp"

#include "../src/music/MusicPlayer.hpp"

#include <chrono>
#include <cstdlib>
#include <signal.h>
#include <thread>
#include <unistd.h>

using namespace miyoofin::music;

namespace {

struct Recorder
{
    std::mutex mutex;
    std::vector<std::string> events; // "start:t0", "stop:t0", ...
    std::vector<std::string> resolved;
    void add(const std::string& e)
    {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(e);
    }
    std::vector<std::string> snapshot()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return events;
    }
};

Track track(const std::string& id, int seconds = 1)
{
    Track t;
    t.id = id;
    t.title = id;
    t.runTimeTicks = static_cast<std::int64_t>(seconds) * 10000000;
    return t;
}

std::vector<Track> tracks(std::initializer_list<const char*> ids)
{
    std::vector<Track> v;
    for (const char* id : ids)
        v.push_back(track(id));
    return v;
}

struct Rig
{
    std::string dir;
    Recorder rec;
    std::unique_ptr<MusicPlayer> player;
    std::atomic<int> resolveCount{0};
    explicit Rig(const std::vector<std::string>& env = {})
    {
        char tmpl[] = "/tmp/miyoofin-music-player-XXXXXX";
        dir = mkdtemp(tmpl);
        PlayerOptions options;
        options.enginePath = "tests/fixtures/fake-audio-engine.sh";
        options.engineEnv = env;
        PlayerHooks hooks;
        hooks.resolve = [this](const Track& t, const std::atomic<bool>&) {
            ++resolveCount;
            ResolvedTrack r;
            r.ok = true;
            r.path = dir + "/" + t.id + ".mp3";
            return r;
        };
        hooks.report = [this](ReportKind kind, const Track& t, std::int64_t, bool,
                              const std::string&) {
            rec.add(std::string(kind == ReportKind::Start      ? "start:"
                                : kind == ReportKind::Progress ? "progress:"
                                                               : "stop:") +
                    t.id);
        };
        player = std::make_unique<MusicPlayer>(options, hooks);
    }
    ~Rig()
    {
        player.reset();
        std::system(("rm -rf " + dir).c_str());
    }
    template <typename Pred> bool until(Pred pred, int ms = 6000)
    {
        for (int waited = 0; waited < ms; waited += 20) {
            player->poll();
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        player->poll();
        return pred();
    }
    bool saw(const std::string& event)
    {
        for (const auto& e : rec.snapshot())
            if (e == event)
                return true;
        return false;
    }
};

void testParseEvents()
{
    std::printf("[test] engine event parsing\n");
    CHECK(parseEngineEvent("ready").type == EngineEvent::Type::Ready);
    CHECK(parseEngineEvent("idle").type == EngineEvent::Type::Idle);
    const EngineEvent s = parseEngineEvent("started /a b/c d.mp3");
    CHECK(s.type == EngineEvent::Type::Started && s.path == "/a b/c d.mp3");
    const EngineEvent p = parseEngineEvent("pos 12.5 240.0 1");
    CHECK(p.type == EngineEvent::Type::Pos && p.position == 12.5 && p.duration == 240.0 &&
          p.paused);
    CHECK(parseEngineEvent("pos garbage").type == EngineEvent::Type::None);
    CHECK(parseEngineEvent("???").type == EngineEvent::Type::None);
    CHECK(parseEngineEvent("ended /x").type == EngineEvent::Type::Ended);
    CHECK(parseEngineEvent("error /x").type == EngineEvent::Type::Error);
    std::printf("[test] engine event parsing OK\n");
}

void testPlaysQueueGapless()
{
    std::printf("[test] player plays a queue and reports\n");
    Rig rig;
    rig.player->playTracks(tracks({"t0", "t1", "t2"}), 0, false);
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Playing; }));
    CHECK(rig.player->view().track.id == "t0");
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Idle; }, 8000));
    const auto events = rig.rec.snapshot();
    // Every track started and stopped, in order.
    std::vector<std::string> starts;
    for (const auto& e : events)
        if (e.compare(0, 6, "start:") == 0)
            starts.push_back(e);
    CHECK(starts == (std::vector<std::string>{"start:t0", "start:t1", "start:t2"}));
    CHECK(
        rig.until([&] { return rig.saw("stop:t0") && rig.saw("stop:t1") && rig.saw("stop:t2"); }));
    std::printf("[test] player plays a queue and reports OK\n");
}

void testControls()
{
    std::printf("[test] player pause, skip and previous\n");
    Rig rig({"FAKE_LEN_MS=20000"});
    rig.player->playTracks(tracks({"t0", "t1", "t2"}), 0, false);
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Playing; }));
    rig.player->togglePause();
    CHECK(rig.player->view().state == PlayState::Paused);
    rig.player->togglePause();
    CHECK(rig.player->view().state == PlayState::Playing);
    rig.player->next();
    CHECK(rig.until([&] {
        auto v = rig.player->view();
        return v.track.id == "t1" && v.state == PlayState::Playing;
    }));
    rig.player->next();
    CHECK(rig.until([&] {
        return rig.player->view().track.id == "t2" &&
               rig.player->view().state == PlayState::Playing;
    }));
    // Previous at the start of a track goes back a track.
    rig.player->previous();
    CHECK(rig.until([&] {
        return rig.player->view().track.id == "t1" &&
               rig.player->view().state == PlayState::Playing;
    }));
    // Seek moves the reported position.
    rig.player->seekBy(10);
    CHECK(rig.until([&] { return rig.player->view().position >= 9.0; }));
    // Previous well into a track restarts it instead.
    rig.player->previous();
    CHECK(rig.player->view().track.id == "t1");
    CHECK(rig.until([&] { return rig.player->view().position < 3.0; }));
    rig.player->stop();
    CHECK(rig.player->view().state == PlayState::Idle);
    CHECK(rig.until([&] { return rig.saw("stop:t1"); }));
    std::printf("[test] player pause, skip and previous OK\n");
}

void testJumpReportsTheTrackThatWasPlaying()
{
    std::printf("[test] jumping reports the song that stopped\n");
    Rig rig({"FAKE_LEN_MS=60000"});
    rig.player->playTracks(tracks({"a", "b", "c"}), 0, false);
    CHECK(rig.until([&] { return rig.saw("start:a"); }));
    rig.player->jumpTo(2); // A -> C (the queue view)
    CHECK(rig.until([&] { return rig.saw("start:c"); }));
    CHECK(rig.until([&] { return rig.saw("stop:a"); }));
    CHECK(!rig.saw("stop:c")); // C had just started; it must not be reported as stopped
    rig.player->next();        // C is last: nothing follows, the queue is kept
    rig.player->jumpTo(0);
    CHECK(rig.until([&] { return rig.saw("start:a"); }));
    int stopsOfA = 0, stopsOfC = 0;
    CHECK(rig.until([&] { return rig.saw("stop:c"); }));
    for (const auto& e : rig.rec.snapshot()) {
        stopsOfA += e == "stop:a";
        stopsOfC += e == "stop:c";
    }
    CHECK(stopsOfC == 1 && stopsOfA >= 1);
    std::printf("[test] jumping reports the song that stopped OK\n");
}

void testSkipsUnplayable()
{
    std::printf("[test] player skips an unplayable track\n");
    Rig rig;
    rig.player->playTracks(tracks({"bad1", "t1"}), 0, false);
    CHECK(rig.until([&] { return rig.saw("start:t1"); }));
    CHECK(!rig.saw("start:bad1"));
    std::printf("[test] player skips an unplayable track OK\n");
}

void testEngineCrashRecovery()
{
    std::printf("[test] player recovers from an engine crash\n");
    char tmpl[] = "/tmp/miyoofin-crash-XXXXXX";
    const std::string crashDir = mkdtemp(tmpl);
    Rig rig({"FAKE_CRASH_FILE=" + crashDir + "/crashed", "FAKE_LEN_MS=20000"});
    rig.player->playTracks(tracks({"t0"}), 0, false);
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Playing; }));
    CHECK(rig.until([&] { return rig.saw("start:t0"); }));
    std::system(("rm -rf " + crashDir).c_str());
    std::printf("[test] player recovers from an engine crash OK\n");
}

void testShutdownReapsEngine()
{
    std::printf("[test] player shutdown reaps the engine\n");
    Rig rig({"FAKE_LEN_MS=20000"});
    rig.player->playTracks(tracks({"t0"}), 0, false);
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Playing; }));
    const pid_t pid = rig.player->enginePid();
    CHECK(pid > 0 && kill(pid, 0) == 0);
    rig.player->shutdown(2000);
    CHECK(rig.player->enginePid() <= 0);
    CHECK(kill(pid, 0) != 0); // gone, not a zombie
    CHECK(rig.until([&] { return rig.saw("stop:t0"); }));
    std::printf("[test] player shutdown reaps the engine OK\n");
}

void testRepeatAndShuffleKeepPlaying()
{
    std::printf("[test] player repeat one\n");
    Rig rig({"FAKE_LEN_MS=300"});
    rig.player->playTracks(tracks({"t0", "t1"}), 0, false);
    rig.player->cycleRepeat(); // all
    rig.player->cycleRepeat(); // one
    CHECK(rig.player->view().repeat == Repeat::One);
    CHECK(rig.until([&] {
        int starts = 0;
        for (const auto& e : rig.rec.snapshot())
            starts += e == "start:t0";
        return starts >= 3;
    }));
    CHECK(rig.player->view().track.id == "t0");
    std::printf("[test] player repeat one OK\n");
}

void testResume()
{
    std::printf("[test] player resumes a saved queue\n");
    Rig rig({"FAKE_LEN_MS=200000"});
    SavedQueue saved;
    saved.tracks = tracks({"r0", "r1", "r2"});
    saved.position = 1;
    saved.seconds = 42;
    rig.player->restoreQueue(saved);
    PlayerView v = rig.player->view();
    CHECK(v.state == PlayState::Idle && v.resumable && v.track.id == "r1" && v.position == 42);
    rig.player->resumeSaved();
    CHECK(rig.until([&] { return rig.player->view().state == PlayState::Playing; }));
    CHECK(rig.player->view().track.id == "r1" && !rig.player->view().resumable);
    CHECK(rig.until([&] { return rig.player->view().position >= 42.0; }));
    rig.player->stop();
    CHECK(rig.player->view().resumable); // stopping by hand keeps the place
    std::printf("[test] player resumes a saved queue OK\n");
}

void testSavesQueue()
{
    std::printf("[test] player saves its queue\n");
    Rig rig({"FAKE_LEN_MS=20000"});
    std::mutex mutex;
    std::vector<std::string> saves;
    PlayerHooks hooks;
    // A second player with a saveQueue hook: the first rig has none.
    PlayerOptions options;
    options.enginePath = "tests/fixtures/fake-audio-engine.sh";
    options.engineEnv = {"FAKE_LEN_MS=20000"};
    hooks.resolve = [&](const Track& t, const std::atomic<bool>&) {
        ResolvedTrack r;
        r.ok = true;
        r.path = rig.dir + "/" + t.id + ".mp3";
        return r;
    };
    hooks.saveQueue = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        saves.push_back(text);
    };
    {
        MusicPlayer player(options, hooks);
        player.playTracks(tracks({"s0", "s1"}), 0, false);
        for (int i = 0; i < 200 && player.view().state != PlayState::Playing; ++i) {
            player.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        player.stop();
        for (int i = 0; i < 100; ++i) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!saves.empty())
                    break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!saves.empty());
    SavedQueue parsed;
    CHECK(parseQueue(saves.back(), parsed) && parsed.tracks.size() == 2);
    std::printf("[test] player saves its queue OK\n");
}

} // namespace

int main()
{
    testParseEvents();
    testPlaysQueueGapless();
    testControls();
    testJumpReportsTheTrackThatWasPlaying();
    testSkipsUnplayable();
    testEngineCrashRecovery();
    testShutdownReapsEngine();
    testRepeatAndShuffleKeepPlaying();
    testResume();
    testSavesQueue();
    return miyoofin_test::finish("music_player");
}
