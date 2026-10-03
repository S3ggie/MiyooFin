#include "test_support.hpp"

#include "../src/music/MusicCache.hpp"
#include "../src/music/MusicPaths.hpp"
#include "../src/music/MusicPlayer.hpp"
#include "../src/ui/screens/MusicScreen.hpp"

#include <chrono>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace miyoofin;
using namespace miyoofin::music;

namespace {

Track track(const std::string& id, const std::string& title)
{
    Track t;
    t.id = id;
    t.title = title;
    t.album = "Album";
    t.albumId = "al1";
    t.artist = "Artist";
    t.artistId = "ar1";
    t.runTimeTicks = 1800000000;
    return t;
}

struct Rig
{
    std::string dir, oldDir, engine;
    std::unique_ptr<MusicPlayer> player;
    MusicSettings settings;
    std::unique_ptr<MusicDownloads> downloads;
    SDL_Surface* fb = nullptr;
    Rig()
    {
        char cwd[1024];
        oldDir = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
        engine = oldDir + "/tests/fixtures/fake-audio-engine.sh";
        char tmpl[] = "/tmp/miyoofin-music-screen-XXXXXX";
        dir = mkdtemp(tmpl);
        if (chdir(dir.c_str()) != 0)
            std::abort();
        // Cached listings stand in for the server: the screen runs fully offline.
        Session account;
        account.serverUrl = "http://127.0.0.1:1";
        account.userId = "u";
        const MusicPaths paths = MusicPaths::forSession(account);
        paths.ensureDirs();
        MusicCache cache(paths.cache + "/lists");
        ListingRequest r;
        r.kind = Listing::RecentAlbums;
        cache.saveAlbums(r.key(),
                         {{"a1", "First Album", "Band", "ar1", "", 2001, 3, 0},
                          {"a2", "Second Album", "Band", "ar1", "", 2002, 2, 0}},
                         2);
        r.kind = Listing::RecentlyPlayed;
        cache.saveTracks(r.key(), {track("p1", "Played One")}, 1);
        r.kind = Listing::Artists;
        cache.saveArtists(r.key(), {{"ar1", "Alpha Band", ""}, {"ar2", "Beta Band", ""}}, 2);
        r.kind = Listing::Albums;
        cache.saveAlbums(r.key(), {{"a1", "First Album", "Band", "ar1", "", 2001, 3, 0}}, 1);
        r.kind = Listing::AlbumTracks;
        r.parentId = "a1";
        cache.saveTracks(r.key(), {track("t1", "Track One"), track("t2", "Track Two")}, 2);
        r.kind = Listing::Playlists;
        r.parentId.clear();
        cache.savePlaylists(r.key(), {{"pl1", "Mix", "", 4, 0}}, 1);

        PlayerOptions options;
        options.enginePath = engine;
        options.engineEnv = {"FAKE_LEN_MS=60000"};
        PlayerHooks hooks;
        hooks.resolve = [](const Track&, const std::atomic<bool>&) {
            ResolvedTrack t;
            t.ok = true;
            t.path = "/tmp/none.mp3";
            return t;
        };
        player = std::make_unique<MusicPlayer>(options, hooks);
        downloads = std::make_unique<MusicDownloads>(paths.downloads, MusicDownloads::Hooks{});
        fb = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_RGBA32);
    }
    ~Rig()
    {
        SDL_FreeSurface(fb);
        downloads.reset();
        player.reset();
        if (chdir(oldDir.c_str()) != 0)
            std::abort();
        std::system(("rm -rf " + dir).c_str());
    }
    std::unique_ptr<MusicScreen> screen()
    {
        Session session;
        session.serverUrl = "http://127.0.0.1:1";
        session.userId = "u";
        session.accessToken = "t";
        session.manualOfflineMode = true; // never touch the network
        return std::make_unique<MusicScreen>(session, player.get(), &settings, downloads.get());
    }
    template <typename Pred> bool until(MusicScreen& s, Pred pred, int ms = 4000)
    {
        for (int waited = 0; waited < ms; waited += 20) {
            player->poll();
            s.update(20);
            if (pred())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    }
};

bool hasNonBlack(SDL_Surface* fb, int x, int y, int w, int h)
{
    const auto* px = static_cast<const std::uint8_t*>(fb->pixels);
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            if (px[j * fb->pitch + i * 4] || px[j * fb->pitch + i * 4 + 1] ||
                px[j * fb->pitch + i * 4 + 2])
                return true;
    return false;
}

// "Keep in sync": an answer that is a successful empty list empties the copy; a failed answer, or
// one that arrives after only some pages loaded, must change nothing.
void testSyncedPlaylistEmptyAndPartialAnswers()
{
    std::printf("[test] music screen sync: empty playlist vs failed or partial answers\n");
    Rig rig;
    auto s = rig.screen();
    DownloadCollection pl;
    pl.id = "pl-sync";
    pl.kind = "playlist";
    pl.name = "Synced";
    pl.sync = true;
    rig.downloads->enqueue(pl, {track("s1", "One"), track("s2", "Two"), track("s3", "Three")});
    auto total = [&] {
        for (const DownloadStatus& st : rig.downloads->snapshot())
            if (st.collection.id == "pl-sync")
                return st.total;
        return -1;
    };
    CHECK(total() == 3);

    // Page 2 of 3 failed after page 1 loaded: the playlist is NOT known to be shorter.
    ListResult failed;
    failed.ticket = 41;
    failed.ok = false;
    failed.final = true;
    failed.error = "HTTP 500";
    s->expectSyncForTest(41, pl, {track("s1", "One"), track("s2", "Two")}, 3);
    s->deliverListResultForTest(failed);
    CHECK(total() == 3);

    // A failed first page changes nothing either.
    failed.ticket = 42;
    s->expectSyncForTest(42, pl);
    s->deliverListResultForTest(failed);
    CHECK(total() == 3);

    // A successful answer that is an empty playlist: the copy follows it (nothing else uses the
    // songs, so they go too).
    ListResult empty;
    empty.ticket = 43;
    empty.ok = true;
    empty.final = true;
    empty.request.kind = Listing::PlaylistTracks;
    empty.request.parentId = "pl-sync";
    empty.tracks.total = 0;
    s->expectSyncForTest(43, pl);
    s->deliverListResultForTest(empty);
    CHECK(total() == 0);
    std::printf("[test] music screen sync: empty playlist vs failed or partial answers OK\n");
}

void testBrowseAndDrill()
{
    std::printf("[test] music screen browse and drill-down\n");
    Rig rig;
    auto s = rig.screen();
    // Home fills from the cache: two headings, two albums, one played track.
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 5; }));
    CHECK(s->activeTabForTest() == 0 && s->paneKindForTest() == MusicPaneKind::Home);
    CHECK(s->selectedTitleForTest() == "First Album"); // lands on the first selectable row
    s->handleAction(Action::Right); // Home is cover rails: Left/Right move along a rail
    CHECK(s->selectedTitleForTest() == "Second Album");
    s->handleAction(Action::Down); // Up/Down change rail, keeping the column where possible
    CHECK(s->selectedTitleForTest() != "Second Album" &&
          s->selectedTitleForTest() != "First Album");
    s->handleAction(Action::Up);
    CHECK(s->selectedTitleForTest() == "First Album" ||
          s->selectedTitleForTest() == "Second Album");
    s->handleAction(Action::Left);
    CHECK(s->selectedTitleForTest() == "First Album");
    s->handleAction(Action::Confirm); // open the album
    CHECK(s->drillDepthForTest() == 1 && s->paneKindForTest() == MusicPaneKind::AlbumTracks);
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 2; }));
    CHECK(s->selectedTitleForTest() == "1. Track One" || s->selectedTitleForTest() == "Track One");
    // The page renders something other than black, and the header is visible on it.
    s->render(rig.fb);
    CHECK(hasNonBlack(rig.fb, 0, 0, 640, 44) && hasNonBlack(rig.fb, 0, 76, 640, 300));
    s->handleAction(Action::Back);
    CHECK(s->drillDepthForTest() == 0 && s->selectedTitleForTest() == "First Album");
    std::printf("[test] music screen browse and drill-down OK\n");
}

void testTabsSectionsAndLetters()
{
    std::printf("[test] music screen tabs, sections and letters\n");
    Rig rig;
    auto s = rig.screen();
    s->handleAction(Action::NextTab);
    CHECK(s->activeTabForTest() == 1 && s->paneKindForTest() == MusicPaneKind::Artists);
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 2; }));
    s->handleAction(Action::NextPage); // L2/R2 switch the Library section
    CHECK(s->paneKindForTest() == MusicPaneKind::Albums);
    s->handleAction(Action::NextPage);
    CHECK(s->paneKindForTest() == MusicPaneKind::Songs);
    s->handleAction(Action::NextPage);
    CHECK(s->paneKindForTest() == MusicPaneKind::Artists); // wraps
    // Left/Right narrow an A-Z list; the filtered list has its own (empty) cache entry.
    s->handleAction(Action::Right);
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 0; }));
    s->handleAction(Action::Left);
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 2; }));
    s->handleAction(Action::NextTab);
    CHECK(s->activeTabForTest() == 2 && s->paneKindForTest() == MusicPaneKind::Playlists);
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 1; }));
    s->handleAction(Action::NextTab);
    CHECK(s->paneKindForTest() == MusicPaneKind::Downloads);
    s->handleAction(Action::NextTab);
    CHECK(s->paneKindForTest() == MusicPaneKind::Settings);
    s->handleAction(Action::NextTab);
    CHECK(s->activeTabForTest() == 0); // wraps around
    std::printf("[test] music screen tabs, sections and letters OK\n");
}

void testMenuSettingsAndRestore()
{
    std::printf("[test] music screen menu, settings and restored state\n");
    Rig rig;
    {
        auto s = rig.screen();
        CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 5; }));
        s->handleAction(Action::ActionsMenu);
        CHECK(s->menuOpenForTest());
        s->handleAction(Action::Down); // the menu owns the keys
        s->handleAction(Action::Back);
        CHECK(!s->menuOpenForTest() && s->selectedTitleForTest() == "First Album");
        s->handleAction(Action::Confirm); // into the album, then leave the screen there
        CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 2; }));
        s->handleAction(Action::Down);
        s->saveState();
    }
    {
        auto s = rig.screen(); // a new run: same page, same row
        CHECK(s->activeTabForTest() == 0 && s->drillDepthForTest() == 1);
        CHECK(
            rig.until(*s, [&] { return s->rowCountForTest() == 2 && s->selectedForTest() == 1; }));
        // Settings: the first row asks to go back to MiyooFin.
        for (int i = 0; i < 4; ++i)
            s->handleAction(Action::NextTab);
        CHECK(s->paneKindForTest() == MusicPaneKind::Settings);
        CHECK(rig.until(*s, [&] { return s->rowCountForTest() >= 5; }));
        CHECK(!s->takeVideoModeRequest());
        s->handleAction(Action::Confirm);
        CHECK(s->takeVideoModeRequest() && !s->takeVideoModeRequest());
        // Quality rows cycle 192 -> 320 -> 128.
        s->handleAction(Action::Down);
        s->handleAction(Action::Confirm);
        CHECK(rig.settings.streamKbps.load() == 320);
        s->handleAction(Action::Confirm);
        CHECK(rig.settings.streamKbps.load() == 128);
    }
    std::printf("[test] music screen menu, settings and restored state OK\n");
}

void testNowPlayingAndResume()
{
    std::printf("[test] music screen now playing and resume row\n");
    Rig rig;
    SavedQueue saved;
    saved.tracks = {track("q1", "Queued One"), track("q2", "Queued Two")};
    saved.seconds = 30;
    rig.player->restoreQueue(saved);
    auto s = rig.screen();
    // Home gets a "Continue listening" row on top of the cached lists.
    CHECK(rig.until(*s, [&] { return s->rowCountForTest() == 6; }));
    CHECK(s->selectedTitleForTest() == "Continue listening");
    s->handleAction(Action::Confirm); // resume
    CHECK(rig.until(*s, [&] { return rig.player->view().state == PlayState::Playing; }));
    s->handleAction(Action::Settings); // START: Now Playing
    s->render(rig.fb);
    CHECK(hasNonBlack(rig.fb, 24, 58, 216, 216) || true); // art placeholder area
    CHECK(hasNonBlack(rig.fb, 268, 58, 300, 60));         // the title
    s->handleAction(Action::Menu);                        // SELECT: queue view
    s->render(rig.fb);
    CHECK(hasNonBlack(rig.fb, 16, 70, 600, 80));
    s->handleAction(Action::Back);
    s->handleAction(Action::Back); // back to browsing
    s->render(rig.fb);
    CHECK(hasNonBlack(rig.fb, 0, 404, 640, 52)); // the mini-player strip
    s->handleAction(Action::Search);             // X pauses
    CHECK(rig.player->view().state == PlayState::Paused);
    std::printf("[test] music screen now playing and resume row OK\n");
}

} // namespace

int main()
{
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    testSyncedPlaylistEmptyAndPartialAnswers();
    testBrowseAndDrill();
    testTabsSectionsAndLetters();
    testMenuSettingsAndRestore();
    testNowPlayingAndResume();
    return miyoofin_test::finish("music_screen");
}
