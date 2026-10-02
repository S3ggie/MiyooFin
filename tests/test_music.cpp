#include "test_support.hpp"

#include "../src/music/MusicApi.hpp"
#include "../src/music/MusicCache.hpp"
#include "../src/music/MusicParse.hpp"
#include "../src/music/MusicQueue.hpp"
#include "../src/music/MusicTracks.hpp"
#include "../src/ui/MusicUiState.hpp"
#include "../src/app/AppMode.hpp"

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

using namespace miyoofin::music;

namespace {

const char* kTrackJson =
    R"({"Name":"Blue \"Monday\"","ServerId":"s","Id":"t1","RunTimeTicks":2412345678,)"
    R"("ProductionYear":1983,"IndexNumber":3,"ParentIndexNumber":2,"Artists":["New Order"],)"
    R"("ArtistItems":[{"Name":"New Order","Id":"ar1"}],"Album":"Power, Corruption & Lies",)"
    R"("AlbumId":"al1","AlbumPrimaryImageTag":"abc","AlbumArtist":"New Order",)"
    R"("ImageTags":{},"UserData":{"IsFavorite":true,"PlayCount":2}})";

const char* kAlbumPage =
    R"({"Items":[{"Name":"Low-Life","Id":"a1","ChildCount":8,"ProductionYear":1985,)"
    R"("AlbumArtist":"New Order","AlbumArtists":[{"Name":"New Order","Id":"ar1"}],)"
    R"("ImageTags":{"Primary":"tag1"},"RunTimeTicks":18000000000},)"
    R"({"Name":"Technique","Id":"a2","ImageTags":{}}],"TotalRecordCount":42,"StartIndex":10})";

Track track(const std::string& id)
{
    Track t;
    t.id = id;
    t.title = id;
    return t;
}

std::vector<Track> tracks(int n)
{
    std::vector<Track> v;
    for (int i = 0; i < n; ++i)
        v.push_back(track("t" + std::to_string(i)));
    return v;
}

void testParsing()
{
    std::printf("[test] music JSON parsing\n");
    const Track t = parseTrack(kTrackJson);
    CHECK(t.id == "t1" && t.title == "Blue \"Monday\"");
    CHECK(t.album == "Power, Corruption & Lies" && t.albumId == "al1");
    CHECK(t.artist == "New Order" && t.artistId == "ar1");
    CHECK(t.trackNumber == 3 && t.discNumber == 2 && t.durationSeconds() == 241);
    CHECK(t.favorite && t.imageTag.empty() && t.albumImageTag == "abc");
    CHECK(t.artId() == "al1" && t.artTag() == "abc"); // art comes from the album

    Page<Album> page;
    CHECK(parseAlbumPage(kAlbumPage, page));
    CHECK(page.items.size() == 2 && page.total == 42 && page.startIndex == 10);
    CHECK(page.hasMore());
    CHECK(page.items[0].title == "Low-Life" && page.items[0].year == 1985 &&
          page.items[0].trackCount == 8 && page.items[0].imageTag == "tag1" &&
          page.items[0].artistId == "ar1");
    CHECK(page.items[1].imageTag.empty() && page.items[1].artist.empty());

    Page<Artist> artists;
    CHECK(parseArtistPage(R"([{"Name":"A","Id":"x"},{"Name":"B","Id":"y"}])", artists));
    CHECK(artists.items.size() == 2 && artists.total == 2 && !artists.hasMore());
    Page<Track> junk;
    CHECK(!parseTrackPage("not json", junk));

    Page<Playlist> playlists;
    CHECK(parsePlaylistPage(R"({"Items":[{"Name":"Mix","Id":"p1","ChildCount":12}],)"
                            R"("TotalRecordCount":1})",
                            playlists));
    CHECK(playlists.items[0].title == "Mix" && playlists.items[0].trackCount == 12);
    // Items without an id are dropped rather than becoming unplayable rows.
    Page<Album> noId;
    CHECK(parseAlbumPage(R"({"Items":[{"Name":"x"}],"TotalRecordCount":1})", noId) &&
          noId.items.empty());
    CHECK(formatDuration(75) == "1:15" && formatDuration(3725) == "1:02:05" &&
          formatDuration(-4) == "0:00");
    std::printf("[test] music JSON parsing OK\n");
}

void testUrls()
{
    std::printf("[test] music URLs\n");
    ListingRequest r;
    r.kind = Listing::Albums;
    r.start = 100;
    r.limit = 50;
    r.letter = 'B';
    const std::string url = buildListingUrl("http://h:8096", "u1", r);
    CHECK(url.find("http://h:8096/Users/u1/Items?Recursive=true") == 0);
    CHECK(url.find("IncludeItemTypes=MusicAlbum") != std::string::npos);
    CHECK(url.find("NameStartsWith=B") != std::string::npos);
    CHECK(url.find("StartIndex=100&Limit=50") != std::string::npos);
    CHECK(url.find("api_key") == std::string::npos && url.find("Token") == std::string::npos);
    r.kind = Listing::AlbumTracks;
    r.parentId = "al 1/x";
    r.letter = 0;
    CHECK(buildListingUrl("http://h", "u", r).find("ParentId=al%201%2Fx") != std::string::npos);
    r.kind = Listing::PlaylistTracks;
    CHECK(buildListingUrl("http://h", "u", r).find("/Playlists/al%201%2Fx/Items?UserId=u") !=
          std::string::npos);
    r.kind = Listing::Artists;
    CHECK(buildListingUrl("http://h", "u", r).find("/Artists/AlbumArtists?UserId=u") !=
          std::string::npos);
    r.letter = '#';
    CHECK(buildListingUrl("http://h", "u", r).find("NameLessThan=A") != std::string::npos);
    CHECK(r.key() == "artists-al 1/x-#");

    CHECK(buildTrackUrl("http://h", "t1", {192}) ==
          "http://h/Audio/t1/stream.mp3?audioCodec=mp3&static=false&audioBitRate=192000"
          "&maxAudioChannels=2");
    CHECK(buildTrackUrl("http://h", "t1", {0}) == "http://h/Items/t1/Download");
    CHECK(std::string(trackExtension({192})) == "mp3" &&
          std::string(trackExtension({0})) == "audio");
    CHECK(buildCoverUrl("http://h/", "a1", "tg", 200).find("http://h/Items/a1/Images/Primary") ==
          0);
    std::printf("[test] music URLs OK\n");
}

void testCache()
{
    std::printf("[test] music list cache\n");
    char tmpl[] = "/tmp/miyoofin-music-cache-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    MusicCache cache(dir + "/lists");

    std::vector<Track> in = tracks(2);
    in[0].title = "Tab\there\nnewline";
    in[0].runTimeTicks = 123456789012;
    in[0].favorite = true;
    in[1].albumImageTag = "tag";
    CHECK(cache.saveTracks("album-tracks-x/y", in, 99));
    std::vector<Track> out;
    int total = 0;
    CHECK(cache.loadTracks("album-tracks-x/y", out, total));
    CHECK(total == 99 && out.size() == 2 && out[0].title == "Tab here newline");
    CHECK(out[0].runTimeTicks == 123456789012 && out[0].favorite && out[1].albumImageTag == "tag");
    CHECK(!cache.loadTracks("missing", out, total));

    std::vector<Album> albums = {{"a1", "T", "Ar", "ar1", "tag", 1999, 11, 77}};
    CHECK(cache.saveAlbums("albums", albums, 1));
    std::vector<Album> albumsOut;
    CHECK(cache.loadAlbums("albums", albumsOut, total) && albumsOut.size() == 1);
    CHECK(albumsOut[0].year == 1999 && albumsOut[0].trackCount == 11 &&
          albumsOut[0].artistId == "ar1");

    std::vector<Artist> artists = {{"x", "X", "t"}};
    std::vector<Artist> artistsOut;
    CHECK(cache.saveArtists("artists", artists, 5) &&
          cache.loadArtists("artists", artistsOut, total));
    CHECK(total == 5 && artistsOut.size() == 1 && artistsOut[0].name == "X");

    std::vector<Playlist> lists = {{"p", "P", "", 3, 9}};
    std::vector<Playlist> listsOut;
    CHECK(cache.savePlaylists("playlists", lists, 1) &&
          cache.loadPlaylists("playlists", listsOut, total));
    CHECK(listsOut[0].trackCount == 3);
    CHECK(MusicCache::fileKey("a/b c") == "a_b_c");
    // A file with the wrong header is ignored, not misparsed.
    FILE* f = std::fopen((dir + "/lists/albums.tsv").c_str(), "w");
    std::fputs("garbage\n", f);
    std::fclose(f);
    CHECK(!cache.loadAlbums("albums", albumsOut, total));
    std::system(("rm -rf " + dir).c_str());
    std::printf("[test] music list cache OK\n");
}

std::string ids(const MusicQueue& q)
{
    std::string s;
    for (int i = 0; i < q.size(); ++i)
        s += (i ? "," : "") + q.at(i)->id;
    return s;
}

void testQueue()
{
    std::printf("[test] music queue\n");
    MusicQueue q;
    CHECK(q.empty() && q.current() == nullptr && !q.advance() && !q.skipNext());
    q.set(tracks(3), 1);
    CHECK(q.current()->id == "t1" && q.position() == 1 && q.peekNext()->id == "t2");
    CHECK(q.advance() && q.current()->id == "t2");
    CHECK(!q.advance() && q.current()->id == "t2"); // repeat off: finished
    CHECK(!q.skipNext());
    CHECK(q.skipPrevious() && q.current()->id == "t1");
    CHECK(q.skipPrevious() && q.skipPrevious() == false && q.current()->id == "t0");

    q.setRepeat(Repeat::All);
    CHECK(q.peekNext()->id == "t1");
    CHECK(q.skipPrevious() && q.current()->id == "t2"); // wraps back
    CHECK(q.advance() && q.current()->id == "t0");
    q.setRepeat(Repeat::One);
    CHECK(q.advance() && q.current()->id == "t0" && q.peekNext()->id == "t0");
    CHECK(q.skipNext() && q.current()->id == "t1"); // the button still moves on
    q.jumpTo(2);
    CHECK(q.skipNext() && q.current()->id == "t0"); // and wraps

    // Shuffle keeps the current track first and plays every track exactly once.
    MusicQueue s;
    s.seed(7);
    s.setShuffle(true);
    s.set(tracks(20), 5);
    CHECK(s.current()->id == "t5" && s.position() == 0);
    std::vector<std::string> seen;
    do
        seen.push_back(s.current()->id);
    while (s.advance());
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    CHECK(seen.size() == 20);
    // Turning shuffle off resumes the source order after the current track.
    s.setShuffle(false);
    const std::string now = s.current()->id;
    CHECK(s.position() == std::atoi(now.c_str() + 1));
    // Repeat-all + shuffle: a new pass never starts with the track that just ended.
    s.setShuffle(true);
    s.setRepeat(Repeat::All);
    s.jumpTo(s.size() - 1);
    const std::string last = s.current()->id;
    CHECK(s.advance() && s.current()->id != last);

    // Editing the queue.
    MusicQueue e;
    e.set(tracks(3), 0);
    e.playNext(track("n"));
    CHECK(ids(e) == "t0,n,t1,t2");
    e.append(track("z"));
    CHECK(ids(e) == "t0,n,t1,t2,z");
    CHECK(!e.removeAt(0)); // the playing track stays
    CHECK(e.removeAt(2) && ids(e) == "t0,n,t2,z");
    e.jumpTo(2);
    CHECK(e.removeAt(0) && e.current()->id == "t2" && e.position() == 1);
    MusicQueue empty;
    empty.playNext(track("only"));
    CHECK(empty.current()->id == "only");
    std::printf("[test] music queue OK\n");
}

void testTrackSource()
{
    std::printf("[test] track source and cache\n");
    char tmpl[] = "/tmp/miyoofin-music-source-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    int fetches = 0;
    bool serveGarbage = false, fail = false;
    TrackSourceConfig cfg;
    cfg.cacheDir = dir + "/stream";
    cfg.quality = {192};
    cfg.cacheLimitBytes = 5000;
    cfg.fetch = [&](const std::string&, const std::string& dest, std::string& error,
                    const std::atomic<bool>&) {
        ++fetches;
        if (fail) {
            error = "boom";
            return false;
        }
        FILE* f = std::fopen(dest.c_str(), "wb");
        std::string body = serveGarbage ? std::string(3000, 'x') : "ID3" + std::string(2997, 'a');
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        return true;
    };
    std::atomic<bool> noCancel{false};
    Track t = track("a1");
    ResolvedTrack r = resolveTrack(cfg, t, noCancel);
    CHECK(r.ok && !r.local && fetches == 1 && r.path == cfg.cacheDir + "/a1-192.mp3");
    CHECK(resolveTrack(cfg, t, noCancel).ok && fetches == 1); // served from the cache
    serveGarbage = true;
    ResolvedTrack bad = resolveTrack(cfg, track("a2"), noCancel);
    CHECK(!bad.ok && bad.error.find("not audio") != std::string::npos);
    CHECK(access((cfg.cacheDir + "/a2-192.mp3.part").c_str(), F_OK) != 0); // no junk left
    serveGarbage = false;
    fail = true;
    CHECK(!resolveTrack(cfg, track("a3"), noCancel).ok);
    fail = false;
    // Pruning: a second 3000-byte file pushes the first out (limit 5000).
    FILE* g = std::fopen((cfg.cacheDir + "/a1-192.mp3").c_str(), "r+b");
    std::fclose(g);
    sleep(1);
    CHECK(resolveTrack(cfg, track("a4"), noCancel).ok);
    CHECK(access((cfg.cacheDir + "/a1-192.mp3").c_str(), F_OK) != 0);
    CHECK(access((cfg.cacheDir + "/a4-192.mp3").c_str(), F_OK) == 0);
    // A finished download wins over the cache and the network.
    const std::string dl = dir + "/downloaded.mp3";
    FILE* d = std::fopen(dl.c_str(), "wb");
    std::fputs("ID3", d);
    std::fclose(d);
    cfg.downloadedPath = [&](const std::string& id) { return id == "a9" ? dl : std::string(); };
    fetches = 0;
    ResolvedTrack local = resolveTrack(cfg, track("a9"), noCancel);
    CHECK(local.ok && local.local && local.path == dl && fetches == 0);
    // Offline: only local/cached files play.
    cfg.offline = [] { return true; };
    cfg.downloadedPath = nullptr;
    ResolvedTrack off = resolveTrack(cfg, track("zz"), noCancel);
    CHECK(!off.ok && off.error == "Not available offline" && fetches == 0);
    std::system(("rm -rf " + dir).c_str());
    std::printf("[test] track source and cache OK\n");
}

void testUiState()
{
    std::printf("[test] music UI state\n");
    using namespace miyoofin;
    MusicUiState s = MusicUiState::defaults();
    CHECK(s.tabs[1].roots.size() == 3 && s.tabs[0].roots.size() == 1);
    s.activeTab = 1;
    s.tabs[1].section = 2;
    s.tabs[1].roots[2].selected = 17;
    s.tabs[1].roots[2].scroll = 12;
    s.tabs[1].roots[2].selectedId = "song-9";
    s.tabs[1].roots[0].letter = 'M';
    MusicFrame album;
    album.kind = MusicPaneKind::AlbumTracks;
    album.id = "al1";
    album.title = "Tab\there";
    album.subtitle = "New Order";
    album.artId = "al1";
    album.artTag = "t";
    album.selected = 3;
    s.tabs[1].drill.push_back(album);
    const std::string text = s.serialize();
    MusicUiState back;
    CHECK(MusicUiState::parse(text, back));
    CHECK(back.activeTab == 1 && back.tabs[1].section == 2);
    CHECK(back.tabs[1].roots[2].selected == 17 && back.tabs[1].roots[2].scroll == 12 &&
          back.tabs[1].roots[2].selectedId == "song-9" && back.tabs[1].roots[0].letter == 'M');
    CHECK(back.tabs[1].drill.size() == 1 && back.tabs[1].drill[0].id == "al1" &&
          back.tabs[1].drill[0].title == "Tab here" && back.tabs[1].drill[0].selected == 3);
    CHECK(back.serialize() == text); // stable across a round trip
    // Garbage and out-of-range values never produce a broken state.
    MusicUiState bad = MusicUiState::defaults();
    CHECK(!MusicUiState::parse("not a state file", bad));
    CHECK(MusicUiState::parse("MFMU=1\ntab\t99\nsection\t1\t42\n", bad));
    CHECK(bad.activeTab == 0 && bad.tabs[1].section == 0);
    CHECK(!MusicUiState::parse("MFMU=1\nroot\t1\t99\t\t\t\t\t\t\t0\t0\t-\n", bad));
    // A drill frame without an id (or of a root kind) is ignored.
    CHECK(MusicUiState::parse("MFMU=1\ndrill\t0\t8\t\ttitle\t\t\t\t\t0\t0\t-\n"
                              "drill\t0\t1\tx\ttitle\t\t\t\t\t0\t0\t-\n",
                              bad) &&
          bad.tabs[0].drill.empty());

    // The remembered mode.
    char tmpl[] = "/tmp/miyoofin-mode-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    CHECK(loadAppMode(dir + "/none") == AppMode::Video);
    CHECK(saveAppMode(AppMode::Music, dir + "/mode") &&
          loadAppMode(dir + "/mode") == AppMode::Music);
    CHECK(saveAppMode(AppMode::Video, dir + "/mode") &&
          loadAppMode(dir + "/mode") == AppMode::Video);
    std::system(("rm -rf " + dir).c_str());
    std::printf("[test] music UI state OK\n");
}

} // namespace

int main()
{
    testParsing();
    testUrls();
    testCache();
    testQueue();
    testTrackSource();
    testUiState();
    return miyoofin_test::finish("music");
}
