// Renders the README's music screenshots from a made-up library: nothing here comes from a
// real server, so no personal data (server address, user, playlists, covers) can appear.
// Run with `make shots-music`; the BMPs land in output/shots/.
#include "../src/music/MusicCache.hpp"
#include "../src/music/MusicPlayer.hpp"
#include "../src/ui/Design.hpp"
#include "../src/ui/screens/MusicScreen.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unistd.h>

using namespace miyoofin;
using namespace miyoofin::music;

namespace {

Track track(const std::string& id, const std::string& title, const std::string& album,
            const std::string& albumId, const std::string& artist, int number, int seconds)
{
    Track t;
    t.id = id;
    t.title = title;
    t.album = album;
    t.albumId = albumId;
    t.artist = artist;
    t.albumArtist = artist;
    t.artistId = "ar-" + artist;
    t.trackNumber = number;
    t.runTimeTicks = static_cast<std::int64_t>(seconds) * 10000000;
    return t;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: shots_music <output-dir>\n");
        return 2;
    }
    design::usePalette(true); // the purple MiyooFin Music look
    char cwd[1024];
    const std::string oldDir = getcwd(cwd, sizeof(cwd)) ? cwd : ".";
    std::string outDir = argv[1];
    if (outDir[0] != '/')
        outDir = oldDir + "/" + outDir;
    const std::string engine = oldDir + "/tests/fixtures/fake-audio-engine.sh";
    char tmpl[] = "/tmp/miyoofin-shots-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    if (chdir(dir.c_str()) != 0)
        return 1;

    // The made-up library.
    const std::vector<std::pair<std::string, std::string>> albumNames = {
        {"First Light", "The Example Band"}, {"Night Drive", "Sample Artist"},
        {"Paper Planes", "Demo Collective"}, {"Slow Tide", "The Example Band"},
        {"Golden Hour", "Sample Artist"},    {"Static Bloom", "Demo Collective"},
        {"Open Roads", "Test Pattern"},      {"Low Light", "Test Pattern"},
        {"Echo Park", "The Example Band"},   {"Blue Hours", "Sample Artist"},
        {"Weekend", "Demo Collective"},      {"Afterglow", "Test Pattern"}};
    std::vector<Album> albums;
    for (std::size_t i = 0; i < albumNames.size(); ++i)
        albums.push_back({"al" + std::to_string(i), albumNames[i].first, albumNames[i].second,
                          "ar-" + albumNames[i].second, "", 2015 + static_cast<int>(i), 10, 0});
    const std::vector<std::string> songNames = {
        "Morning Static", "City Lights", "Paper Boats", "Second Wind", "Long Way Home",
        "Quiet Hours",    "Neon Rain",   "Daydream",    "Open Window", "Last Train"};
    std::vector<Track> albumTracks;
    for (std::size_t i = 0; i < songNames.size(); ++i)
        albumTracks.push_back(track("t" + std::to_string(i), songNames[i], "First Light", "al0",
                                    "The Example Band", static_cast<int>(i) + 1, 180 + 7 * i));
    std::vector<Track> played;
    for (std::size_t i = 0; i < 8; ++i)
        played.push_back(track("p" + std::to_string(i), songNames[(i * 3) % songNames.size()],
                               albumNames[i].first, "al" + std::to_string(i), albumNames[i].second,
                               1, 200));

    MusicCache cache("music-cache/lists");
    ListingRequest r;
    r.kind = Listing::RecentAlbums;
    cache.saveAlbums(r.key(), albums, static_cast<int>(albums.size()));
    r.kind = Listing::RecentlyPlayed;
    cache.saveTracks(r.key(), played, static_cast<int>(played.size()));
    r.kind = Listing::Albums;
    cache.saveAlbums(r.key(), albums, static_cast<int>(albums.size()));
    r.kind = Listing::Artists;
    cache.saveArtists(r.key(),
                      {{"ar-The Example Band", "The Example Band", ""},
                       {"ar-Sample Artist", "Sample Artist", ""},
                       {"ar-Demo Collective", "Demo Collective", ""},
                       {"ar-Test Pattern", "Test Pattern", ""}},
                      4);
    r.kind = Listing::Songs;
    cache.saveTracks(r.key(), albumTracks, static_cast<int>(albumTracks.size()));
    r.kind = Listing::AlbumTracks;
    r.parentId = "al0";
    cache.saveTracks(r.key(), albumTracks, static_cast<int>(albumTracks.size()));
    r.kind = Listing::Playlists;
    r.parentId.clear();
    cache.savePlaylists(r.key(),
                        {{"pl1", "Road Trip Mix", "", 42, 0},
                         {"pl2", "Focus", "", 18, 0},
                         {"pl3", "Weekend Playlist", "", 64, 0},
                         {"pl4", "Throwbacks", "", 120, 0}},
                        4);

    PlayerOptions options;
    options.enginePath = engine;
    options.engineEnv = {"FAKE_LEN_MS=240000"};
    PlayerHooks hooks;
    hooks.resolve = [](const Track&, const std::atomic<bool>&) {
        ResolvedTrack t;
        t.ok = true;
        t.path = "/tmp/none.mp3";
        return t;
    };
    MusicPlayer player(options, hooks);
    MusicSettings settings;
    MusicDownloads downloads("music-downloads", MusicDownloads::Hooks{});
    SDL_Surface* fb = SDL_CreateRGBSurfaceWithFormat(0, 640, 480, 32, SDL_PIXELFORMAT_RGBA32);

    Session session;
    session.serverUrl = "http://127.0.0.1:1";
    session.userId = "u";
    session.accessToken = "t";
    session.userName = "demo";
    session.manualOfflineMode =
        false; // the cached lists stand in for a server; refreshes just fail
    MusicScreen screen(session, &player, &settings, &downloads);

    auto settle = [&](int ms) {
        for (int waited = 0; waited < ms; waited += 20) {
            player.poll();
            screen.update(20);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    auto shot = [&](const char* name) {
        settle(300);
        screen.render(fb);
        const std::string path = outDir + "/" + name + ".bmp";
        SDL_SaveBMP(fb, path.c_str());
        std::printf("wrote %s\n", path.c_str());
    };
    auto press = [&](Action a, int times = 1) {
        for (int i = 0; i < times; ++i) {
            screen.handleAction(a);
            settle(120);
        }
    };

    settle(600);
    shot("music-home");
    press(Action::NextTab); // Library: artists
    shot("music-artists");
    settings.albumGrid.store(true);
    press(Action::NextPage); // Albums section
    shot("music-albums");
    settings.albumGrid.store(false);
    press(Action::Confirm); // an album's page
    shot("music-album");
    // Start playing the album, then Now Playing, lyrics and the queue.
    player.playTracks(albumTracks, 2, false);
    for (int i = 0; i < 100 && player.view().state != PlayState::Playing; ++i)
        settle(50);
    settle(1500);
    press(Action::Settings); // START: Now Playing
    shot("music-nowplaying");
    settle(12000); // far enough in for the synced lyrics to have a current line
    screen.showLyricsForTest({{12000, "Morning light on the window"},
                              {16000, "Coffee going cold again"},
                              {20000, "I keep counting all the roads"},
                              {24000, "That lead me back to you"},
                              {28000, "Paper boats on the water"},
                              {32000, "Drifting out of view"}});
    screen.update(20);
    shot("music-lyrics");
    press(Action::Back); // leave the lyrics
    press(Action::Menu); // SELECT: the queue
    shot("music-queue");
    press(Action::Back);
    press(Action::Back);
    press(Action::NextTab); // Playlists
    shot("music-playlists");
    press(Action::NextTab, 2); // Settings
    shot("music-settings");

    SDL_FreeSurface(fb);
    if (chdir(oldDir.c_str()) != 0)
        return 1;
    std::system(("rm -rf " + dir).c_str());
    return 0;
}
