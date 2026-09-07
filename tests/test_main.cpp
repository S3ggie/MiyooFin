// Checkpoint B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a — tests for authentication, session
// persistence, device identity, URL normalisation, B4 JSON parsing/tab
// building, B5a artwork infrastructure, B5b selected artwork loading,
// B5c1 per-type artwork box dimensions, B5d1 row card geometry + scrolling,
// B5d2a row artwork loading state, B5e1a season parsing groundwork,
// B5e2a episode parsing groundwork, B5e3b initial episode focus,
// B5f2 playback request writing, and B5f3a in-process external playback
// handoff semantics.
// All tests are pure logic (no network calls).
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <string>
#include "miyoofin/version.hpp"
#include "../src/net/JellyfinApi.hpp"
#include "../src/net/ArtworkUrl.hpp"
#include "../src/net/Session.hpp"
#include "../src/net/DeviceIdentity.hpp"
#include "../src/data/MediaItem.hpp"
#include "../src/ui/BitmapFont.hpp"
#include "../src/ui/screens/HomeScreen.hpp"
#include "../src/ui/screens/ServerEntryScreen.hpp"
#include "../src/ui/screens/LoginScreen.hpp"
#include "../src/ui/OnScreenKeyboard.hpp"
#include "../src/image/ImageDecoder.hpp"
#include "../src/cache/ImageCache.hpp"
#include "../src/cache/LibraryCache.hpp"
#include "../src/cache/SyncState.hpp"
#include "../src/cache/OfflineCatalog.hpp"
#include "../src/cache/OfflineLibraryProjection.hpp"
#include "../src/net/HttpClient.hpp"
#include "../src/net/RouteRequest.hpp"
#include "../src/net/RouteStatus.hpp"
#include "../src/net/ServerAddress.hpp"
#include "../src/net/ClockCheck.hpp"
#include "../src/ui/ArtworkLayout.hpp"
#include "../src/ui/MovieTitle.hpp"
#include "../src/ui/ShowsBrowser.hpp"
#include "../src/ui/screens/EpisodeBrowserScreen.hpp"
#include "../src/ui/screens/SeriesScreen.hpp"
#include "../src/ui/screens/MovieDetailsScreen.hpp"
#include "../src/app/ScreenStack.hpp"
#include "../src/app/UiDiagnostics.hpp"
#include "../src/playback/PlaybackRequest.hpp"
#include "../src/playback/OfflinePlaybackJournal.hpp"
#include "../src/download/DownloadTypes.hpp"
#include "../src/download/DownloadManager.hpp"
#include "../src/download/DownloadSupport.hpp"
#include "../src/download/DownloadReconcile.hpp"
#include "../src/download/DownloadUi.hpp"
#include "../src/input/InputManager.hpp"
#include <unistd.h>
#include <sys/stat.h>

using namespace miyoofin;

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            std::printf("  FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                        __FILE__, __LINE__, std::string(b).c_str(), std::string(a).c_str()); \
            ++g_failures; \
        } \
    } while (0)

static void testRouteRequest()
{
    std::printf("[test] LAN route selection and fallback\n");
    RouteStatus::record(ApiRoute::Unknown);
    CHECK(RouteStatus::latest()==ApiRoute::Unknown);
    Session s; s.serverUrl="https://public";
    std::string error; int calls=0;
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;return base=="https://public";},error));
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    CHECK(calls==1 && RouteRequest(s).primary()=="https://public");
    s.localServerUrl="http://lan"; calls=0; error.clear();
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;return base=="http://lan";},error));
    CHECK(calls==1 && RouteRequest(s).primary()=="http://lan");
    CHECK(RouteStatus::latest()==ApiRoute::Lan);
    calls=0; error.clear();
    CHECK(RouteRequest(s).run([&](const std::string &base){++calls;if(base=="http://lan"){error="Transport: Couldn't connect";return false;}return base=="https://public";},error));
    CHECK(calls==2);
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){
        ++calls;
        error="Transport: Couldn't connect";
        return false;
    },error));
    CHECK(calls==2);
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){++calls;error="Unauthorized";return false;},error));
    CHECK(calls==1);
    calls=0; error.clear();
    CHECK(!RouteRequest(s).run([&](const std::string &){++calls;error="Transport: Callback aborted";return false;},error));
    CHECK(calls==1); // cancellation must never launch a public retry
    CHECK(RouteStatus::latest()==ApiRoute::Public);
    const std::string scope=LibraryCache::scopeKey(s.serverUrl,"user");
    CHECK_EQ(scope,LibraryCache::scopeKey("https://public","user"));
    CHECK_EQ(RouteRequest::replaceBase("http://lan/a","http://lan","https://public"),"https://public/a");

    Session lanCanonical; lanCanonical.serverUrl="http://192.168.1.212:8096";
    lanCanonical.publicServerUrl="https://public.example.com";
    const std::string canonical=lanCanonical.serverUrl;
    const std::string lanScope=LibraryCache::scopeKey(lanCanonical.serverUrl,"user");
    calls=0; error.clear();
    CHECK(RouteRequest(lanCanonical).run([&](const std::string &base){++calls;if(base==canonical){error="Transport: Couldn't connect";return false;}return base=="https://public.example.com";},error));
    CHECK(calls==2);
    CHECK_EQ(lanCanonical.serverUrl,canonical);
    CHECK_EQ(LibraryCache::scopeKey(lanCanonical.serverUrl,"user"),lanScope);

    RouteStatus::record(ApiRoute::Unknown);
    Session lanOnly; lanOnly.serverUrl="http://192.168.1.212:8096";
    CHECK(RouteRequest(lanOnly).run([&](const std::string &base){return base=="http://192.168.1.212:8096";},error));
    CHECK(RouteStatus::latest()==ApiRoute::Lan);

    RouteStatus::record(ApiRoute::Unknown);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"UNKNOWN");
    RouteStatus::record(ApiRoute::Lan);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"LAN");
    RouteStatus::record(ApiRoute::Public);
    CHECK_EQ(std::string(HomeScreen::lastApiRouteValue()),"PUBLIC");
}

#include "cases/test_ui_foundation.inc"
static void testDownloadInterruptStates()
{
    std::printf("[test] download interrupt state mapping\n");
    CHECK(stateAfterInterrupt(DownloadInterrupt::Playback) == DownloadState::PausedForPlayback);
    CHECK(stateAfterInterrupt(DownloadInterrupt::UserPause) == DownloadState::Paused);
    CHECK(stateAfterInterrupt(DownloadInterrupt::Cancel) == DownloadState::Failed);
    CHECK(stateAfterInterrupt(DownloadInterrupt::Shutdown) == DownloadState::Failed);
}

static MediaItem titledMovie(const std::string &id, const std::string &title)
{
    MediaItem item;
    item.id = id;
    item.title = title;
    item.type = "movie";
    return item;
}

static void testMovieOrganizationalTitles()
{
    std::printf("[test] movie organizational titles\n");
    CHECK_EQ(movieOrganizationalTitle("The Amazing Spider-Man"), "Amazing Spider-Man");
    CHECK_EQ(movieOrganizationalTitle("The Batman"), "Batman");
    CHECK_EQ(movieOrganizationalTitle("the matrix"), "matrix");
    CHECK_EQ(movieOrganizationalTitle("THE MATRIX"), "MATRIX");
    CHECK_EQ(movieOrganizationalTitle("The Thing"), "Thing");
    CHECK_EQ(movieOrganizationalTitle("The"), "The");
    CHECK_EQ(movieOrganizationalTitle("There Will Be Blood"), "There Will Be Blood");
    CHECK_EQ(movieOrganizationalTitle("Theater Camp"), "Theater Camp");
    CHECK_EQ(movieOrganizationalTitle("The "), "The ");
    CHECK_EQ(movieOrganizationalTitle(""), "");
}

static void testMovieAlphabetOrganization()
{
    std::printf("[test] movie alphabet organization\n");
    CHECK(movieMatchesAlphabetFilter("The Amazing Spider-Man", 0));
    CHECK(movieMatchesAlphabetFilter("The Batman", 1));
    CHECK(movieMatchesAlphabetFilter("The Dark Knight", 3));
    CHECK(movieMatchesAlphabetFilter("The Lord of the Rings", 11));
    CHECK(movieMatchesAlphabetFilter("The", 19));
    CHECK(movieMatchesAlphabetFilter("The Thing", 19));
    CHECK(movieMatchesAlphabetFilter("There Will Be Blood", 19));
    CHECK(movieMatchesAlphabetFilter("Theater Camp", 19));
    CHECK(!movieMatchesAlphabetFilter("The Amazing Spider-Man", 19));
}

static void testMovieOrganizationalSort()
{
    std::printf("[test] movie organizational sort\n");
    std::vector<MediaItem> movies = {
        titledMovie("1", "The Batman"), titledMovie("2", "Alien"),
        titledMovie("3", "The Amazing Spider-Man"), titledMovie("4", "Avatar"),
        titledMovie("5", "The Dark Knight"), titledMovie("6", "Batman"),
        titledMovie("7", "The Lord of the Rings")};
    std::sort(movies.begin(), movies.end(), movieOrganizationalLess);
    const std::vector<std::string> expected = {
        "Alien", "The Amazing Spider-Man", "Avatar", "Batman", "The Batman",
        "The Dark Knight", "The Lord of the Rings"};
    for (size_t i = 0; i < expected.size(); ++i) CHECK_EQ(movies[i].title, expected[i]);
}

static void testMovieAlphabetFocus()
{
    std::printf("[test] movie alphabet focus\n");
    CHECK(movieAlphabetFocus("The Amazing Spider-Man") == 0);
    CHECK(movieAlphabetFocus("The Batman") == 1);
    CHECK(movieAlphabetFocus("The") == 19);
    CHECK(movieAlphabetFocus("There Will Be Blood") == 19);
    CHECK(movieAlphabetFocus("1917") == 0);
}

static MediaItem titledShow(const std::string &id, const std::string &title, const std::string &genre="") { MediaItem i; i.id=id;i.title=title;i.type="show";if(!genre.empty())i.genres={genre};return i; }
static void testShowsPresentation()
{
    std::printf("[test] shows title, anime classification, and grid helpers\n");
    CHECK_EQ(organizationalTitle("The Boys"),"Boys"); CHECK_EQ(organizationalTitle("The Last of Us"),"Last of Us"); CHECK_EQ(organizationalTitle("The Simpsons"),"Simpsons"); CHECK_EQ(organizationalTitle("The"),"The"); CHECK_EQ(organizationalTitle("There She Goes"),"There She Goes");
    CHECK(libraryNameContainsAnimeToken("Anime"));CHECK(libraryNameContainsAnimeToken("My Anime"));CHECK(libraryNameContainsAnimeToken("Shows - Anime"));CHECK(!libraryNameContainsAnimeToken("Animated Shows"));CHECK(!libraryNameContainsAnimeToken("Animation"));
    CachedLibraryView normal{"n","TV Shows","tvshows",{titledShow("b","The Boys"),titledShow("bb","Breaking Bad"),titledShow("dup","Normal")}}; CachedLibraryView anime{"a","Anime Shows","tvshows",{titledShow("at","Attack on Titan"),titledShow("ap","The Apothecary Diaries"),titledShow("dup","Duplicate")}}; ShowsPresentation p=makeShowsPresentation({normal,anime});CHECK(p.shows.size()==2&&p.anime.size()==3);CHECK_EQ(p.shows[0].title,"The Boys");CHECK_EQ(p.shows[1].title,"Breaking Bad");CHECK_EQ(p.anime[0].title,"The Apothecary Diaries");CHECK(matchesAlphabetFilter(p.anime[0].title,0));CHECK(!matchesAlphabetFilter(p.anime[0].title,19));
    CHECK(moveShowsGrid(3,8,0,1)==3);CHECK(moveShowsGrid(3,8,1,0)==7);CHECK(moveShowsGrid(7,9,1,0)==8);CHECK(clampShowsGridScroll(12,50,0)==1);CHECK(closestShowsGridIndex(11,5)==4);
}

// -------------------------------------------------------------------
// Test 1: URL normalisation (from B2, kept)
// -------------------------------------------------------------------
#include "cases/test_api_session.inc"
#include "cases/test_artwork_episode.inc"
static void testPlaybackRequestMovie()
{
    std::printf("[test] B5f2: PlaybackRequest valid movie\n");
    const char *tmpPath = "test_playback_movie.txt";
    std::remove(tmpPath);

    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "abc123", "movie",
                                   18822664360LL, error));
    CHECK(PlaybackRequest::existsAt(tmpPath));

    // Read back and verify
    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=abc123\n") != std::string::npos);
    CHECK(content.find("item_type=movie\n") != std::string::npos);
    CHECK(content.find("resume_ticks=18822664360\n") != std::string::npos);
    CHECK(content.find("access_token") == std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f2: PlaybackRequest valid movie OK\n");
}

// B5f2: PlaybackRequest — valid episode writes expected fields
static void testPlaybackRequestEpisode()
{
    std::printf("[test] B5f2: PlaybackRequest valid episode\n");
    const char *tmpPath = "test_playback_episode.txt";
    std::remove(tmpPath);

    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "ep-42", "episode", 0, error));
    CHECK(PlaybackRequest::existsAt(tmpPath));

    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=ep-42\n") != std::string::npos);
    CHECK(content.find("item_type=episode\n") != std::string::npos);
    CHECK(content.find("resume_ticks=0\n") != std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f2: PlaybackRequest valid episode OK\n");
}

// B5f2: PlaybackRequest — empty ID rejected
static void testPlaybackRequestEmptyId()
{
    std::printf("[test] B5f2: PlaybackRequest empty ID rejected\n");
    std::string error;
    CHECK(!PlaybackRequest::writeTo("/tmp/bpr_test.txt", "", "movie", 0, error));
    CHECK(!error.empty());
    std::printf("[test] B5f2: PlaybackRequest empty ID rejected OK\n");
}

// B5f2: PlaybackRequest — empty type rejected
static void testPlaybackRequestEmptyType()
{
    std::printf("[test] B5f2: PlaybackRequest empty type rejected\n");
    std::string error;
    CHECK(!PlaybackRequest::writeTo("/tmp/bpr_test2.txt", "id1", "", 0, error));
    CHECK(!error.empty());
    std::printf("[test] B5f2: PlaybackRequest empty type rejected OK\n");
}

// B5f2: PlaybackRequest — remove
static void testPlaybackRequestRemove()
{
    std::printf("[test] B5f2: PlaybackRequest remove\n");
    const char *tmpPath = "test_playback_rm.txt";
    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "x", "movie", -42, error));
    {
        FILE *f = std::fopen(tmpPath, "r");
        CHECK(f != nullptr);
        char buf[256] = {};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        CHECK(std::string(buf).find("resume_ticks=0\n") != std::string::npos);
    }
    CHECK(PlaybackRequest::existsAt(tmpPath));
    CHECK(PlaybackRequest::removeAt(tmpPath));
    CHECK(!PlaybackRequest::existsAt(tmpPath));
    // Remove when not existing should not fail
    CHECK(PlaybackRequest::removeAt(tmpPath));
    std::printf("[test] B5f2: PlaybackRequest remove OK\n");
}

static void writePlaybackResultFixture(const char *path,
                                       const char *content)
{
    FILE *f = std::fopen(path, "w");
    CHECK(f != nullptr);
    if (f) {
        CHECK(std::fputs(content, f) >= 0);
        CHECK(std::fclose(f) == 0);
    }
}

static void testPlaybackResultParsing()
{
    std::printf("[test] playback result strict 64-bit parsing\n");
    const char *path = "test_playback_result.txt";
    std::int64_t ticks = -1;
    std::string error;

    writePlaybackResultFixture(path,
        "item_id=abc\nposition_ticks=24758110000\n");
    CHECK(PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 24758110000LL);
    CHECK(!PlaybackRequest::existsAt(path));

    writePlaybackResultFixture(path, "item_id=abc\nposition_ticks=0\n");
    CHECK(PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 0);

    const char *invalid[] = {
        "item_id=abc\n",
        "item_id=abc\nposition_ticks=12oops\n",
        "item_id=abc\nposition_ticks=-1\n",
        "item_id=abc\nposition_ticks=9223372036854775808\n",
        "position_ticks=10\n"
    };
    for (const char *content : invalid) {
        writePlaybackResultFixture(path, content);
        ticks = 777;
        CHECK(!PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
        CHECK(ticks == 777);
        CHECK(!PlaybackRequest::existsAt(path));
    }

    writePlaybackResultFixture(path, "item_id=other\nposition_ticks=99\n");
    ticks = 777;
    CHECK(!PlaybackRequest::consumeResultFrom(path, "abc", ticks, error));
    CHECK(ticks == 777);
    CHECK(!PlaybackRequest::existsAt(path));
    std::printf("[test] playback result strict 64-bit parsing OK\n");
}

static void testPlaybackResultDelay()
{
    std::printf("[test] playback result one-update delay\n");
    bool pending = true;
    int delayUpdates = 1;
    CHECK(!PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    CHECK(pending && delayUpdates == 0);
    CHECK(PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    CHECK(!pending);
    CHECK(!PlaybackRequest::advanceResultConsumption(pending, delayUpdates));
    std::printf("[test] playback result one-update delay OK\n");
}
static void testOfflinePlaybackJournal(){std::string p="/tmp/miyoofin-journal-"+std::to_string((long long)getpid());OfflinePlaybackEntry a;a.itemId="a";a.itemType="movie";a.baseServerTicks=10;a.finalTicks=30;a.localTimestamp=1;CHECK(OfflinePlaybackJournal::upsert(p,a));a.finalTicks=40;CHECK(OfflinePlaybackJournal::upsert(p,a));std::vector<OfflinePlaybackEntry> got;CHECK(OfflinePlaybackJournal::load(p,got));CHECK(got.size()==(size_t)1&&got[0].finalTicks==40);CHECK(decideOfflineSync(10,10,true)==OfflineSyncDecision::Push);CHECK(decideOfflineSync(10,20,true)==OfflineSyncDecision::Conflict);CHECK(decideOfflineSync(10,0,false)==OfflineSyncDecision::Retry);PlaybackResult r;std::string e;CHECK(PlaybackRequest::parseResult("item_id=a\nposition_ticks=5\n",r,e)&&r.serverReported);CHECK(PlaybackRequest::parseResult("item_id=a\nitem_type=movie\nposition_ticks=5\nbase_resume_ticks=2\nsource_mode=local\nserver_reported=0\n",r,e)&&!r.serverReported&&r.baseResumeTicks==2);std::vector<OfflinePlaybackEntry> v(1,a);int submitted=0;auto pushed=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=10;return OfflineJournalRequestStatus::Success;},[&](const OfflinePlaybackEntry&){++submitted;return OfflineJournalRequestStatus::Success;});CHECK(pushed.pushed==1&&v.empty()&&submitted==1);v.assign(1,a);auto transient=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Transient;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(transient.retry&&v.size()==(size_t)1);v.assign(1,a);auto conflict=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=11;return OfflineJournalRequestStatus::Success;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(conflict.conflicts==1&&v[0].conflict);submitted=0;auto skipped=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&t){t=10;return OfflineJournalRequestStatus::Success;},[&](const OfflinePlaybackEntry&){++submitted;return OfflineJournalRequestStatus::Success;});CHECK(submitted==0&&v.size()==(size_t)1);v.assign(1,a);auto missing=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Missing;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(missing.changed&&!missing.retry&&v.size()==(size_t)1&&v[0].serverMissing);CHECK(OfflinePlaybackJournal::save(p,v));CHECK(OfflinePlaybackJournal::discardMissing(p,"a"));got.clear();CHECK(OfflinePlaybackJournal::load(p,got)&&got.empty());v.assign(1,a);auto unauthorized=syncOfflinePlaybackEntries(v,[](const OfflinePlaybackEntry&,std::int64_t&){return OfflineJournalRequestStatus::Unauthorized;},[](const OfflinePlaybackEntry&){return OfflineJournalRequestStatus::Success;});CHECK(unauthorized.retry&&unauthorized.unauthorized&&v.size()==(size_t)1);}

// -------------------------------------------------------------------
// B5f3a: In-process external playback handoff tests
// -------------------------------------------------------------------

// B5f3a: ScreenStack external playback flag — initially false
static void testExternalPlaybackFlagInitial()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag initially false\n");
    ScreenStack stack;
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag initially false OK\n");
}

// B5f3a: ScreenStack external playback flag — set and consume
static void testExternalPlaybackFlagSetConsume()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag set/consume\n");
    ScreenStack stack;
    stack.requestExternalPlayback();
    CHECK(stack.pollExternalPlayback());
    // After consuming, should be false
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag set/consume OK\n");
}

// B5f3a: ScreenStack external playback flag — multiple sets collapse
static void testExternalPlaybackFlagMultipleSet()
{
    std::printf("[test] B5f3a: ScreenStack external playback flag multiple sets\n");
    ScreenStack stack;
    stack.requestExternalPlayback();
    stack.requestExternalPlayback();  // redundant set
    CHECK(stack.pollExternalPlayback());
    CHECK(!stack.pollExternalPlayback());
    std::printf("[test] B5f3a: ScreenStack external playback flag multiple sets OK\n");
}

// B5f3a: PlaybackRequest still writes item_id and item_type (no exit code 42)
static void testPlaybackRequestStillWorks()
{
    std::printf("[test] B5f3a: PlaybackRequest still writes correctly\n");
    const char *tmpPath = "test_b5f3a_request.txt";
    std::remove(tmpPath);

    std::string error;
    CHECK(PlaybackRequest::writeTo(tmpPath, "movie-99", "movie", 123, error));

    // Verify contents
    FILE *f = std::fopen(tmpPath, "r");
    CHECK(f != nullptr);
    char buf[256] = {};
    std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);

    std::string content(buf);
    CHECK(content.find("item_id=movie-99\n") != std::string::npos);
    CHECK(content.find("item_type=movie\n") != std::string::npos);
    CHECK(content.find("resume_ticks=123\n") != std::string::npos);
    // Must not contain access_token
    CHECK(content.find("access_token") == std::string::npos);

    std::remove(tmpPath);
    std::printf("[test] B5f3a: PlaybackRequest still writes correctly OK\n");
}

// B5f3a: ScreenStack does not get destroyed during external playback
// (conceptual test — push screens, set flag, verify stack is intact)
static void testScreenStackPreservedDuringExternalPlayback()
{
    std::printf("[test] B5f3a: ScreenStack preserved during external playback\n");
    // We can't easily instantiate real screens without SDL, but we can
    // test that the stack size and empty state are unaffected by the flag.
    ScreenStack stack;
    CHECK(stack.empty());
    CHECK(stack.size() == 0);

    // Set the external playback flag
    stack.requestExternalPlayback();

    // Stack should still be empty (flag doesn't modify stack)
    CHECK(stack.empty());
    CHECK(stack.size() == 0);

    // Consume the flag
    CHECK(stack.pollExternalPlayback());

    // Stack still empty, flag consumed
    CHECK(stack.empty());
    std::printf("[test] B5f3a: ScreenStack preserved during external playback OK\n");
}

class RetirementTestScreen : public Screen {
public:
    RetirementTestScreen(bool deferred, std::atomic<bool> *left=nullptr,
                         std::atomic<bool> *destroyed=nullptr)
        : m_deferred(deferred), m_left(left), m_destroyed(destroyed) {}
    ~RetirementTestScreen() override {
        if (m_deferred) usleep(200000);
        if (m_destroyed) m_destroyed->store(true);
    }
    void enter() override {}
    void leave() override { if (m_left) m_left->store(true); }
    bool handleAction(Action) override { return false; }
    void update(Uint32) override {}
    void render(SDL_Surface *) override {}
    bool deferDestruction() const override { return m_deferred; }
private:
    bool m_deferred;
    std::atomic<bool> *m_left;
    std::atomic<bool> *m_destroyed;
};

static void testScreenRetirementDoesNotBlockPop()
{
    std::printf("[test] ScreenStack deferred worker retirement\n");
    std::atomic<bool> left{false}, destroyed{false};
    ScreenStack stack;
    stack.push(std::unique_ptr<Screen>(new RetirementTestScreen(false)));
    stack.push(std::unique_ptr<Screen>(new RetirementTestScreen(true,&left,&destroyed)));
    const auto start=std::chrono::steady_clock::now();
    CHECK(stack.pop());
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count();
    CHECK(left.load());
    CHECK(elapsed < 100);
    CHECK(stack.size()==1);
    for(int i=0;i<50&&!destroyed.load();++i)usleep(10000);
    CHECK(destroyed.load());
    std::printf("[test] ScreenStack deferred worker retirement OK\n");
}

static void testMovieDetailsOpensBeforeArtworkPreparation()
{
    std::printf("[test] MovieDetailsScreen asynchronous first state\n");
    Session session;
    session.serverUrl="http://127.0.0.1:1";
    session.accessToken="test-token";
    session.deviceId="test-device";
    MediaItem movie=titledMovie("movie-async","Async Movie");
    movie.imageTags["Primary"]="uncached-artwork";
    MovieDetailsScreen screen(session,movie);
    CHECK(std::string(screen.diagnosticName())=="MovieDetailsScreen");
    CHECK(screen.deferDestruction());
    const auto start=std::chrono::steady_clock::now();
    screen.enter();
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now()-start).count();
    // enter() only owns/starts the cancellable worker. Cache I/O, HTTP and
    // JPEG decode cannot delay the first interactive state.
    CHECK(elapsed < 100);
    CHECK(screen.handleAction(Action::Right));
    SDL_Surface *fb=SDL_CreateRGBSurface(0,640,480,32,
        0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    CHECK(fb!=nullptr);
    if(fb){
        const auto renderStart=std::chrono::steady_clock::now();
        screen.render(fb);
        const auto renderElapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-renderStart).count();
        CHECK(renderElapsed < 100);
        SDL_FreeSurface(fb);
    }
    screen.leave();
    std::printf("[test] MovieDetailsScreen asynchronous first state OK\n");
}

static void testMovieDetailsUsesGridArtworkImmediately()
{
    std::printf("[test] MovieDetailsScreen grid artwork fallback\n");
    Session session;
    MediaItem movie=titledMovie("movie-grid-art","Grid Artwork");
    auto artwork=std::make_shared<DecodedImage>();
    artwork->width=64;
    artwork->height=96;
    artwork->pixels.assign(64*96*4,0);
    for(size_t i=0;i<artwork->pixels.size();i+=4){
        artwork->pixels[i]=17; artwork->pixels[i+1]=34;
        artwork->pixels[i+2]=51; artwork->pixels[i+3]=255;
    }
    MovieDetailsScreen screen(session,movie,{},artwork);
    SDL_Surface *fb=SDL_CreateRGBSurface(0,640,480,32,
        0x000000FF,0x0000FF00,0x00FF0000,0xFF000000);
    CHECK(fb!=nullptr);
    if(fb){
        screen.render(fb);
        Uint8 r=0,g=0,b=0,a=0;
        const Uint32 pixel=*reinterpret_cast<Uint32*>(
            static_cast<Uint8*>(fb->pixels) + 50*fb->pitch + 30*4);
        SDL_GetRGBA(pixel,fb->format,&r,&g,&b,&a);
        CHECK(r==17 && g==34 && b==51 && a==255);
        SDL_FreeSurface(fb);
    }
    std::printf("[test] MovieDetailsScreen grid artwork fallback OK\n");
}

static void testDpadHoldRepeatTiming()
{
    std::printf("[test] D-pad hold repeat timing\n");
    InputManager::DpadRepeatState state;

    // Fresh press emits immediately; duplicate/native repeat does not.
    CHECK(InputManager::beginDpadPress(state, Action::Right, 100));
    CHECK(state.held);
    CHECK(state.nextRepeatAt == 400);
    CHECK(!InputManager::beginDpadPress(state, Action::Right, 200));
    CHECK(state.nextRepeatAt == 400);

    CHECK(!InputManager::takeDpadRepeat(state, 399));
    CHECK(InputManager::takeDpadRepeat(state, 400));
    CHECK(!InputManager::takeDpadRepeat(state, 489));
    CHECK(InputManager::takeDpadRepeat(state, 490));

    // A late poll emits once and advances beyond now, with no catch-up burst.
    CHECK(InputManager::takeDpadRepeat(state, 1000));
    CHECK(!InputManager::takeDpadRepeat(state, 1000));

    InputManager::endDpadPress(state);
    CHECK(!state.held);
    CHECK(!InputManager::takeDpadRepeat(state, 2000));

    InputManager::DpadRepeatState wrapping;
    CHECK(InputManager::beginDpadPress(
        wrapping, Action::Left, 0xffffff00u));
    CHECK(!InputManager::takeDpadRepeat(wrapping, 43));
    CHECK(InputManager::takeDpadRepeat(wrapping, 44));

    CHECK(InputManager::isDpadRepeatAction(Action::Up));
    CHECK(InputManager::isDpadRepeatAction(Action::Down));
    CHECK(InputManager::isDpadRepeatAction(Action::Left));
    CHECK(InputManager::isDpadRepeatAction(Action::Right));
    CHECK(!InputManager::isDpadRepeatAction(Action::Confirm));
    CHECK(!InputManager::isDpadRepeatAction(Action::Back));
    CHECK(!InputManager::isDpadRepeatAction(Action::PrevTab));
    CHECK(!InputManager::isDpadRepeatAction(Action::Raw));

    InputManager::DpadRepeatState nonDirectional;
    CHECK(!InputManager::beginDpadPress(
        nonDirectional, Action::Confirm, 0));

    std::array<InputManager::DpadRepeatState, 4> states;
    for (auto &held : states)
        CHECK(InputManager::beginDpadPress(held, Action::Down, 0));
    InputManager::resetDpadRepeatStates(states);
    for (const auto &cleared : states) CHECK(!cleared.held);
    std::printf("[test] D-pad hold repeat timing OK\n");
}

static MediaItem cacheItem(const std::string &id) { MediaItem i; i.id=id;i.title="\xCE\xA9 \"title\"";i.overview="line one\nline two";i.year=2024;i.rating=8.5f;i.genre="Drama";i.type="movie";i.etag="etag-"+id;i.genres={"Drama","Sci-Fi"};i.played=true;i.progress=.5f;i.playbackPositionTicks=987654;i.imageTags["Primary"]="tag-"+id;i.indexNumber=2;i.parentIndexNumber=3;i.runTimeTicks=10000000;i.seriesName="series";i.seriesId="sid";i.seasonId="seid";i.artR=1;i.artG=2;i.artB=3;return i; }
static bool sameItem(const MediaItem&a,const MediaItem&b){return a.id==b.id&&a.title==b.title&&a.overview==b.overview&&a.year==b.year&&a.rating==b.rating&&a.genre==b.genre&&a.type==b.type&&a.etag==b.etag&&a.genres==b.genres&&a.played==b.played&&a.progress==b.progress&&a.playbackPositionTicks==b.playbackPositionTicks&&a.imageTags==b.imageTags&&a.indexNumber==b.indexNumber&&a.parentIndexNumber==b.parentIndexNumber&&a.runTimeTicks==b.runTimeTicks&&a.seriesName==b.seriesName&&a.seriesId==b.seriesId&&a.seasonId==b.seasonId&&a.artR==b.artR&&a.artG==b.artG&&a.artB==b.artB;}
static void testLibraryCacheNew(){std::string d="/tmp/miyoofin-cache-test";mkdir(d.c_str(),0755);std::string p=d+"/x.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";for(int i=0;i<500;i++)v.items.push_back(cacheItem(std::to_string(i)));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show")}});CHECK(LibraryCache::save(p,s));LibrarySnapshot got;CHECK(LibraryCache::load(p,got));CHECK(got.movies.size()==1&&got.movies[0].items.size()==500&&sameItem(got.movies[0].items[0],s.movies[0].items[0]));FILE*f=fopen(p.c_str(),"r+b");fputc('X',f);fclose(f);LibrarySnapshot keep=s;CHECK(!LibraryCache::load(p,keep));CHECK(keep.movies[0].items.size()==500);}
static void testLibraryCacheV3SaveLoad(){std::printf("[test] LibraryCache v3 save/load and needsRefresh\n");std::string d="/tmp/miyoofin-cache-v3-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";v.items.push_back(cacheItem("m1"));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show1")}});s.continueWatching.push_back(cacheItem("cw1"));s.recentlyAdded.push_back(cacheItem("ra1"));CHECK(LibraryCache::save(p,s));LibrarySnapshot got;bool needsRefresh=true;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(!needsRefresh);CHECK(got.movies.size()==1&&got.movies[0].items.size()==1);CHECK(sameItem(got.movies[0].items[0],s.movies[0].items[0]));CHECK(got.shows.size()==1&&got.shows[0].items.size()==1);CHECK(sameItem(got.shows[0].items[0],s.shows[0].items[0]));CHECK(got.continueWatching.size()==1);CHECK(got.recentlyAdded.size()==1);CHECK(sameItem(got.continueWatching[0],s.continueWatching[0]));CHECK(sameItem(got.recentlyAdded[0],s.recentlyAdded[0]));std::printf("[test] LibraryCache v3 save/load and needsRefresh OK\n");}
static void testLibraryCacheV2BackCompat(){std::printf("[test] LibraryCache v2 back-compat loads with needsRefresh\n");std::string d="/tmp/miyoofin-cache-v2-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";v.items.push_back(cacheItem("m1"));s.movies.push_back(v);s.shows.push_back({"s","Shows","tvshows",{cacheItem("show1")}});s.continueWatching.push_back(cacheItem("cw1"));s.recentlyAdded.push_back(cacheItem("ra1"));CHECK(LibraryCache::save(p,s));{FILE *f=fopen(p.c_str(),"r+b");CHECK(f!=nullptr);fseek(f,4,SEEK_SET);unsigned char v2[4]={2,0,0,0};fwrite(v2,1,4,f);fclose(f);}LibrarySnapshot got;bool needsRefresh=false;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(needsRefresh);CHECK(got.movies.size()==1&&got.movies[0].items.size()==1);CHECK(sameItem(got.movies[0].items[0],s.movies[0].items[0]));CHECK(got.continueWatching.size()==1);CHECK(got.recentlyAdded.size()==1);std::printf("[test] LibraryCache v2 back-compat loads with needsRefresh OK\n");}
static void testLibraryCacheV1BackCompat(){std::printf("[test] LibraryCache v1 back-compat loads with needsRefresh\n");std::string d="/tmp/miyoofin-cache-v1-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";std::vector<unsigned char> b;auto u32=[&b](uint32_t n){for(int i=0;i<4;i++)b.push_back((unsigned char)(n>>(i*8)));};auto str=[&u32,&b](const std::string& s){u32((uint32_t)s.size());b.insert(b.end(),s.begin(),s.end());};b={'M','F','L','C'};u32(1);u32(1);str("v1mov");str("Movies");str("movies");u32(0);u32(1);str("v1shw");str("Shows");str("tvshows");u32(0);FILE*f=fopen(p.c_str(),"wb");CHECK(f!=nullptr);fwrite(b.data(),1,b.size(),f);fclose(f);LibrarySnapshot got;bool needsRefresh=false;CHECK(LibraryCache::load(p,got,nullptr,&needsRefresh));CHECK(needsRefresh);CHECK(got.movies.size()==1);CHECK(got.movies[0].id=="v1mov");CHECK(got.movies[0].items.empty());CHECK(got.shows.size()==1);CHECK(got.shows[0].id=="v1shw");CHECK(got.shows[0].items.empty());CHECK(got.continueWatching.empty());CHECK(got.recentlyAdded.empty());std::printf("[test] LibraryCache v1 back-compat loads with needsRefresh OK\n");}
static void testLibraryCacheUnknownVersion(){std::printf("[test] LibraryCache unknown version rejected\n");std::string d="/tmp/miyoofin-cache-unk-test-"+std::to_string((long long)getpid());mkdir(d.c_str(),0755);std::string p=d+"/snap.bin";LibrarySnapshot s;CachedLibraryView v;v.id="v";v.name="Movies";v.collectionType="movies";s.movies.push_back(v);CHECK(LibraryCache::save(p,s));{FILE*f=fopen(p.c_str(),"r+b");CHECK(f!=nullptr);fseek(f,4,SEEK_SET);unsigned char bad[4]={99,0,0,0};fwrite(bad,1,4,f);fclose(f);}LibrarySnapshot got;bool needsRefresh=true;CHECK(!LibraryCache::load(p,got,nullptr,&needsRefresh));std::printf("[test] LibraryCache unknown version rejected OK\n");}
static bool librarySyncShouldFetch(bool haveCache,bool cacheNeedsRefresh,bool haveSyncState,bool syncFresh){return !haveCache||cacheNeedsRefresh||!haveSyncState||!syncFresh;}
static void testLibraryCacheFetchDecision(){std::printf("[test] librarySyncShouldFetch decision logic\n");CHECK(!librarySyncShouldFetch(true,false,true,true));CHECK(librarySyncShouldFetch(true,false,true,false));CHECK(librarySyncShouldFetch(true,true,true,true));CHECK(librarySyncShouldFetch(true,true,true,false));CHECK(librarySyncShouldFetch(false,false,true,true));CHECK(librarySyncShouldFetch(false,false,false,false));CHECK(librarySyncShouldFetch(false,false,true,false));std::printf("[test] librarySyncShouldFetch decision logic OK\n");}
static void testSyncState(){std::string p="/tmp/miyoofin-sync-state-"+std::to_string((long long)getpid())+"/state";SyncState a;a.lastSuccessfulMs=1000;a.lastReconcileMs=800;CHECK(SyncStateStore::save(p,a));SyncState b;CHECK(SyncStateStore::load(p,b)&&b.lastSuccessfulMs==1000&&b.lastReconcileMs==800);CHECK(syncStateFresh(b,1000+15*60*1000-1,15*60*1000));CHECK(!syncStateFresh(b,1000+15*60*1000,15*60*1000));FILE*f=fopen(p.c_str(),"wb");CHECK(f!=nullptr);if(f){fputs("broken",f);fclose(f);}SyncState keep=b;CHECK(!SyncStateStore::load(p,keep));CHECK(keep.lastSuccessfulMs==1000);}
static std::string readFixture(const std::string &path){std::string out;FILE*f=fopen(path.c_str(),"rb");if(!f)return out;char b[128];size_t n;while((n=fread(b,1,sizeof(b),f)))out.append(b,n);fclose(f);return out;}
static void writeFixture(const std::string &path,const char *data,size_t size){FILE *f=fopen(path.c_str(),"wb");CHECK(f!=nullptr);if(f){CHECK(fwrite(data,1,size,f)==size);fclose(f);}}
static void testHlsDownloadStore(){
    std::printf("[test] HLS manifest and segment storage\n");
    std::string root="/tmp/miyoofin-hls-"+std::to_string((long long)getpid()); DownloadStore store(root); DownloadItem h;
    h.itemId="hls-item";h.mediaSourceId="source";h.expectedSize=999;h.chunkSize=1;h.hlsStorage=true;h.hlsSegmentCount=0;h.hlsProfile="test-profile";h.state=DownloadState::Downloading;
    // Playlist discovery is durable before segment zero has completed.
    h.hlsSegmentCount=3;
    CHECK(store.ensureHlsDirectories("scope",h.itemId)); CHECK(store.saveManifest("scope",h)); DownloadItem loaded; CHECK(store.loadManifest("scope",h.itemId,loaded));
    CHECK(loaded.hlsStorage&&loaded.itemId==h.itemId&&loaded.mediaSourceId=="source"&&loaded.hlsSegmentCount==3&&loaded.hlsProfile=="test-profile");
    DownloadItem legacy=h;legacy.itemId="legacy";legacy.hlsStorage=false;legacy.expectedSize=1; CHECK(store.saveManifest("scope",legacy)); CHECK(readFixture(store.manifestPath("scope",h.itemId)).rfind("MFDM=2\n",0)==0); CHECK(readFixture(store.manifestPath("scope",legacy.itemId)).rfind("MFDM=1\n",0)==0);
    writeFixture(store.segmentPath("scope",h.itemId,0),"abc",3); writeFixture(store.segmentPath("scope",h.itemId,1),"de",2); writeFixture(store.segmentPath("scope",h.itemId,2,true),"x",1);
    CHECK(store.isCompleteSegment("scope",h.itemId,0)); CHECK(!store.isCompleteSegment("scope",h.itemId,2)); CHECK(store.firstIncompleteSegment("scope",h)==2); CHECK(::access(store.segmentPath("scope",h.itemId,2,true).c_str(),F_OK)==0); // interrupted part is retained
    // A retry starts the same incomplete segment from a fresh .part and must
    // not alter media that was already completed before the failed attempt.
    writeFixture(store.segmentPath("scope",h.itemId,2,true),"proxy error body",16);
    writeFixture(store.segmentPath("scope",h.itemId,2,true),"retry-media",11);
    CHECK(readFixture(store.segmentPath("scope",h.itemId,2,true))=="retry-media");
    CHECK(readFixture(store.segmentPath("scope",h.itemId,0))=="abc");
    store.reconcile("scope",h); CHECK(h.downloadedBytes==5); CHECK(!store.validateCompletedDownload("scope",h)); // missing segment prevents completion
    writeFixture(store.segmentPath("scope",h.itemId,2),"fghi",4); CHECK(store.firstIncompleteSegment("scope",h)==3); store.reconcile("scope",h); CHECK(h.downloadedBytes==9); CHECK(store.validateCompletedDownload("scope",h)); CHECK(h.state==DownloadState::Complete);
    std::printf("[test] HLS manifest and segment storage OK\n");
}
static DownloadItem restartFixture(const std::string &id, DownloadState state=DownloadState::Queued) {
    DownloadItem item; item.itemId=id; item.chunkSize=1; item.hlsStorage=true;
    item.hlsSegmentCount=2; item.hlsProfile="test"; item.state=state; return item;
}
static void testDownloadRestartPersistence(){
    std::printf("[test] download restart persistence\n");
    const std::string root="/tmp/miyoofin-restart-"+std::to_string((long long)getpid());
    DownloadStore store(root);
    // Enqueue is UI-safe, while the worker persists each manifest before the
    // shared index. Playback holds transfers back so both remain queued.
    { DownloadManager manager({},root); manager.setPlaybackActive(true);
      DownloadItem one=restartFixture("queued-one"), two=restartFixture("queued-two");
      manager.enqueue({one,two});
      for(unsigned n=0;n<100;n++){std::vector<DownloadItem> saved;if(store.loadIndex("anonymous",saved,nullptr)&&saved.size()==2)break;usleep(10000);}
      std::vector<DownloadItem> saved; CHECK(store.loadIndex("anonymous",saved)); CHECK(saved.size()==2);
      for(const auto &item:saved){DownloadItem manifest;CHECK(store.loadManifest("anonymous",item.itemId,manifest));CHECK(manifest.state==DownloadState::Queued);}
    }
    { DownloadManager restarted({},root); restarted.setPlaybackActive(true); auto snapshot=restarted.snapshot();
      CHECK(snapshot.items.size()==2); }

    // A corrupt index which loaded one manifest before finding a missing one
    // rebuilds from a clean vector; it cannot duplicate the first item.
    DownloadItem active=restartFixture("active",DownloadState::Downloading), queued=restartFixture("queued",DownloadState::Queued);
    CHECK(store.ensureHlsDirectories("fixture",active.itemId)); CHECK(store.ensureHlsDirectories("fixture",queued.itemId));
    writeFixture(store.segmentPath("fixture",active.itemId,0),"partial",7);
    CHECK(store.saveManifest("fixture",active)); CHECK(store.saveManifest("fixture",queued));
    DownloadItem recovered, stillQueued; CHECK(store.loadManifest("fixture","active",recovered)); CHECK(store.reconcile("fixture",recovered)); CHECK(recovered.state==DownloadState::Queued);
    CHECK(store.loadManifest("fixture","queued",stillQueued)); CHECK(store.reconcile("fixture",stillQueued)); CHECK(stillQueued.state==DownloadState::Queued);
    FILE *bad=fopen((store.scopePath("fixture")+"/index.v1").c_str(),"wb"); CHECK(bad!=nullptr); if(bad){fputs("MFDI=1\nactive\nmissing\n",bad);fclose(bad);}
    Session session;session.serverUrl="http://fixture";session.userId="user";
    { DownloadManager restarted(session,root); restarted.setPlaybackActive(true); auto snapshot=restarted.snapshot();
      CHECK(snapshot.items.size()==2); std::set<std::string> ids; for(const auto&i:snapshot.items)ids.insert(i.itemId); CHECK(ids.size()==2); }
    CHECK(store.isCompleteSegment("fixture","active",0));
    std::printf("[test] download restart persistence OK\n");
}
static void testOfflineCatalog(){std::string root="/tmp/miyoofin-offline-catalog-test-"+std::to_string((long long)getpid()),p=root+"/catalog";MediaItem s=cacheItem("series");s.title="Séries 世界";MediaItem season=cacheItem("season");season.seriesId=s.id;MediaItem ep=cacheItem("episode");ep.seriesId=s.id;ep.seasonId=season.id;ep.indexNumber=2;ep.parentIndexNumber=1;
    // Season planning stores its series, season and episodes without requiring a screen visit.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));OfflineCatalogSnapshot x;CHECK(OfflineCatalog::load(p,x));CHECK(x.series[s.id].title==s.title&&x.seasonsBySeries[s.id][0].id==season.id);auto &got=x.episodesBySeason[season.id][0];CHECK(got.id==ep.id&&got.indexNumber==2&&got.parentIndexNumber==1&&got.runTimeTicks==ep.runTimeTicks&&got.playbackPositionTicks==ep.playbackPositionTicks);
    // Series planning merges all fetched seasons/episodes, retains other cached series, and de-duplicates IDs.
    MediaItem other=cacheItem("other-series"),season2=cacheItem("season-2"),ep2=cacheItem("episode-2");season2.seriesId=s.id;ep2.seriesId=s.id;ep2.seasonId=season2.id;CHECK(OfflineCatalog::storeSeasons(p,other,{cacheItem("other-season")}));CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season,season2,season2},{{season.id,{ep,ep}},{season2.id,{ep2,ep2}}}));CHECK(OfflineCatalog::load(p,x));CHECK(x.series.count(other.id)==1&&x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1&&x.episodesBySeason[season2.id].size()==(size_t)1);
    // An incomplete planner result is a no-op and cannot erase valid cached hierarchy.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{},{{season.id,{}}},false));CHECK(OfflineCatalog::load(p,x));CHECK(x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1);
    // A successful authoritative top-level listing prunes deleted series and
    // their hierarchy only; failed/incomplete refreshes above never do.
    CHECK(OfflineCatalog::reconcileSeries(p,{other}));CHECK(OfflineCatalog::load(p,x));CHECK(!x.series.count(s.id)&&x.series.count(other.id)&&!x.seasonsBySeries.count(s.id)&&!x.episodesBySeason.count(season.id));
    // Corrupting metadata never rewrites it or touches a real downloaded-item fixture.
    DownloadStore store(root+"/downloads");std::string scope="fixture",itemId="downloaded-item";DownloadItem downloaded;downloaded.itemId=itemId;downloaded.expectedSize=5;downloaded.chunkSize=5;downloaded.state=DownloadState::Complete;downloaded.downloadedBytes=5;CHECK(store.saveManifest(scope,downloaded));CHECK(store.saveIndex(scope,{downloaded}));std::string chunks=store.itemPath(scope,itemId)+"/chunks";CHECK(mkdir(chunks.c_str(),0755)==0);FILE*chunk=fopen(store.chunkPath(scope,itemId,0).c_str(),"wb");fwrite("hello",1,5,chunk);fclose(chunk);std::string manifestBefore=readFixture(store.manifestPath(scope,itemId)),chunkBefore=readFixture(store.chunkPath(scope,itemId,0));FILE*f=fopen(p.c_str(),"wb");fputs("bad",f);fclose(f);std::string catalogBefore=readFixture(p);OfflineCatalogSnapshot keep=x;CHECK(!OfflineCatalog::load(p,keep));CHECK(!OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));CHECK(readFixture(p)==catalogBefore);CHECK(readFixture(store.manifestPath(scope,itemId))==manifestBefore);CHECK(readFixture(store.chunkPath(scope,itemId,0))==chunkBefore);DownloadItem loaded;CHECK(store.loadManifest(scope,itemId,loaded));CHECK(store.validateCompletedDownload(scope,loaded));std::vector<DownloadItem> index;CHECK(store.loadIndex(scope,index));CHECK(index.size()==(size_t)1&&index[0].itemId==itemId);}
static void testSeasonPosterScheduling(){
    std::string old=ImageCache::cacheDir(); ImageCache::setCacheDir("/tmp/miyoofin-season-poster-test/");
    MediaItem season=cacheItem("season-poster"); season.type="season"; MediaItem duplicate=season, episode=cacheItem("episode-thumb"); episode.type="episode";
    auto jobs=HomeScreen::collectSeasonPosterJobs({season,duplicate});
    CHECK(jobs.size()==(size_t)1 && jobs[0].width==74 && jobs[0].height==111 && jobs[0].itemId==season.id);
    unsigned char bytes[1]={0}; CHECK(ImageCache::writeToCache(season.id,ImageType::Primary,season.imageTags["Primary"],74,111,bytes,1));
    CHECK(HomeScreen::collectSeasonPosterJobs({season}).empty());
    // The global hierarchy prefetch accepts only seasons; episode thumbnails
    // stay exclusively in EpisodeBrowserScreen's visible/on-demand queue.
    CHECK(HomeScreen::collectSeasonPosterJobs({episode}).empty());
    ImageCache::removeCached(season.id,ImageType::Primary,season.imageTags["Primary"],74,111); ImageCache::setCacheDir(old);
}

static void testOfflineLibraryProjection(){
    LibrarySnapshot l; MediaItem movie=cacheItem("movie"),partial=cacheItem("partial"),show=cacheItem("show"),anime=cacheItem("anime"),season=cacheItem("s1"),empty=cacheItem("s2"),ep1=cacheItem("e1"),ep3=cacheItem("e3"),remote=cacheItem("remote");
    movie.type=partial.type="movie"; movie.playbackPositionTicks=50; show.type=anime.type="show"; show.title="Show"; anime.title="Anime"; season.seriesId="stale-series"; empty.seriesId.clear();
    ep1.type=ep3.type=remote.type="episode"; ep1.seriesId=ep3.seriesId=remote.seriesId=show.id; ep1.seasonId=ep3.seasonId=remote.seasonId=season.id; ep1.indexNumber=1; ep3.indexNumber=3; ep1.playbackPositionTicks=25;
    l.movies={{"m","Movies","movies",{movie,partial}}}; l.shows={{"tv","Shows","tvshows",{show}},{"a","Anime","tvshows",{anime}}}; l.continueWatching={remote,movie,ep1,partial}; l.recentlyAdded={remote};
    // The map key is authoritative: stale and missing season parents are
    // normalized before applying the complete-download visibility filter.
    OfflineCatalogSnapshot c; c.series[show.id]=show; c.series[anime.id]=anime; c.seasonsBySeries[show.id]={season,empty}; c.episodesBySeason[season.id]={ep1,ep3,remote};
    DownloadSnapshot d; DownloadItem dm; dm.itemId=movie.id; dm.itemType="movie"; dm.state=DownloadState::Complete; dm.updatedAt=20;
    DownloadItem de1; de1.itemId=ep1.id; de1.itemType="episode"; de1.state=DownloadState::Complete; de1.updatedAt=30; de1.seriesId=show.id; de1.seasonId=season.id;
    DownloadItem de3=de1; de3.itemId=ep3.id; de3.updatedAt=10;
    DownloadItem failed=dm; failed.itemId=partial.id; failed.state=DownloadState::Failed;
    DownloadItem queued=dm; queued.itemId="queued"; queued.state=DownloadState::Queued;
    DownloadItem paused=dm; paused.itemId="partial"; paused.state=DownloadState::Paused;
    d.items={dm,de1,de3,failed,queued,paused};
    for(int n=0;n<8;++n) { DownloadItem extra=dm; extra.itemId="extra-"+std::to_string(n); extra.title="Extra"; extra.updatedAt=40+n; d.items.push_back(extra); }
    OfflineLibraryProjection p(l,c,d);
    CHECK(p.movies().size()==1&&p.movies()[0].id==movie.id); CHECK(p.series().size()==1&&p.series()[0].id==show.id); CHECK(p.seasons(show.id).size()==1&&p.seasons(show.id)[0].id==season.id); CHECK(p.episodes(season.id).size()==2&&p.episodes(season.id)[1].id==ep3.id); CHECK(!p.playable(partial.id));
    // Online retains Home; offline has no Home projection or curation surface.
    auto online=tabsFromSnapshot(l);
    auto offline=offlineTabsFromSnapshot(l);
    const auto onlineNames=tabNames(online), offlineNames=tabNames(offline);
    CHECK((onlineNames==std::vector<std::string>{"Home","Movies","Shows","Downloads","Settings"}));
    CHECK((offlineNames==std::vector<std::string>{"Movies","Shows","Downloads","Settings"}));
    CHECK(std::find(offlineNames.begin(),offlineNames.end(),"Home")==offlineNames.end());
    // Cold offline startup and a selected Home both use the Movies fallback.
    CHECK(transitionTabIndex({},0,offline)==0);
    CHECK(transitionTabIndex(online,0,offline)==0);
    // Other named tabs survive layout changes where possible.
    CHECK(transitionTabIndex(online,2,offline)==1);
    CHECK(transitionTabIndex(online,3,offline)==2);
    CHECK(transitionTabIndex(online,4,offline)==3);
    CHECK(transitionTabIndex(offline,0,online)==1);
    CHECK(transitionTabIndex(offline,1,online)==2);
    CHECK(transitionTabIndex(offline,2,online)==3);
    CHECK(transitionTabIndex(offline,3,online)==4);
}
static void testSettingsRowActions(){
    CHECK(HomeScreen::settingsRowCount() == 9);
    CHECK(HomeScreen::settingsRowAction(0)==HomeScreen::SettingsRowAction::OfflineMode);
    CHECK(HomeScreen::settingsRowAction(1)==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK(HomeScreen::settingsRowAction(2)==HomeScreen::SettingsRowAction::LocalAddress);
    CHECK(HomeScreen::settingsRowAction(3)==HomeScreen::SettingsRowAction::None);
    CHECK(HomeScreen::settingsRowAction(7)==HomeScreen::SettingsRowAction::None);
    CHECK(HomeScreen::settingsRowAction(8)==HomeScreen::SettingsRowAction::Logout);
    CHECK(HomeScreen::settingsRowAction(9)==HomeScreen::SettingsRowAction::None);
}

static void testLanServerAddressClassificationAndSettingsLayout(){
    std::printf("[test] LAN server address classification and Settings layout\n");
    CHECK(isObviousLanServerUrl("http://10.0.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://172.16.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://172.31.255.255:8096"));
    CHECK(isObviousLanServerUrl("http://192.168.1.212:8096"));
    CHECK(isObviousLanServerUrl("http://127.0.0.1:8096"));
    CHECK(isObviousLanServerUrl("http://LOCALHOST:8096"));
    CHECK(!isObviousLanServerUrl("http://172.15.255.255:8096"));
    CHECK(!isObviousLanServerUrl("http://172.32.0.1:8096"));
    CHECK(!isObviousLanServerUrl("http://8.8.8.8:8096"));
    CHECK(!isObviousLanServerUrl("https://public.example.com"));
    CHECK(!isObviousLanServerUrl("http://jellyfin.local:8096"));
    CHECK(!isObviousLanServerUrl("http://192.168.1.212:70000"));
    CHECK(!isObviousLanServerUrl("not a url"));

    Session lanOnly; lanOnly.serverUrl="http://192.168.1.212:8096";
    const auto lanRows=HomeScreen::settingsAddressRows(lanOnly);
    CHECK(lanRows.size()==2);
    CHECK_EQ(lanRows[0].section,"LAN Server");
    CHECK_EQ(lanRows[0].value,"http://192.168.1.212:8096");
    CHECK(lanRows[0].action==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK_EQ(lanRows[1].section,"Public Address");
    CHECK_EQ(lanRows[1].value,"Not Set");
    CHECK(lanRows[1].action==HomeScreen::SettingsRowAction::PublicAddress);
    CHECK(HomeScreen::settingsRowCount(lanOnly)==10);
    CHECK(HomeScreen::settingsRowAction(1,lanOnly)==HomeScreen::SettingsRowAction::ChangeServer);
    CHECK(HomeScreen::settingsRowAction(2,lanOnly)==HomeScreen::SettingsRowAction::PublicAddress);
    lanOnly.publicServerUrl="https://public.example.com";
    CHECK_EQ(HomeScreen::settingsAddressRows(lanOnly)[1].value,"https://public.example.com");

    Session publicOnly; publicOnly.serverUrl="https://public.example.com";
    const auto publicRows=HomeScreen::settingsAddressRows(publicOnly);
    CHECK(publicRows.size()==2);
    CHECK_EQ(publicRows[0].section,"Public Server");
    CHECK_EQ(publicRows[0].value,"https://public.example.com");
    CHECK_EQ(publicRows[1].section,"Local Address");
    CHECK_EQ(publicRows[1].value,"Not Set");
    CHECK(HomeScreen::settingsRowAction(2,publicOnly)==HomeScreen::SettingsRowAction::LocalAddress);

    Session dualRoute=publicOnly; dualRoute.localServerUrl="http://192.168.1.212:8096";
    const auto dualRows=HomeScreen::settingsAddressRows(dualRoute);
    CHECK(dualRows.size()==2);
    CHECK_EQ(dualRows[0].section,"Public Server");
    CHECK_EQ(dualRows[1].section,"Local Address");
    CHECK_EQ(dualRows[1].value,"http://192.168.1.212:8096");
}
static void testSeriesCachedSeasonHandoff(){
    Session session; MediaItem show=cacheItem("show"),cached=cacheItem("cached"),fresh=cacheItem("fresh"); show.type="show"; cached.type=fresh.type="season";
    // A nonempty RAM handoff is ready before enter(), so push has no catalog I/O.
    SeriesScreen warm(session,show,{},false,{cached}); CHECK(warm.diagnosticSeasonsReady()); CHECK(warm.diagnosticSeasons().size()==(size_t)1&&warm.diagnosticSeasons()[0].id==cached.id);
    // With no handoff, the constructor preserves the existing Loading state.
    SeriesScreen cold(session,show); CHECK(!cold.diagnosticSeasonsReady());
    // The later network completion still replaces the handed-off hierarchy.
    int selected=0; auto replaced=SeriesScreen::replaceSeasonsKeepingSelection({cached},{fresh},selected); CHECK(replaced.size()==(size_t)1&&replaced[0].id==fresh.id&&selected==0);
    // Offline callers hand over the existing downloaded-only projection, never
    // its raw catalog seasons.
    MediaItem remote=cacheItem("remote"); remote.type="season"; OfflineCatalogSnapshot catalog; catalog.seasonsBySeries[show.id]={cached,remote}; MediaItem episode=cacheItem("episode"); episode.type="episode"; episode.seriesId=show.id; episode.seasonId=cached.id; catalog.episodesBySeason[cached.id]={episode}; DownloadItem download; download.itemId=episode.id; download.state=DownloadState::Complete; DownloadSnapshot downloads; downloads.items={download}; LibrarySnapshot library; OfflineLibraryProjection projection(library,catalog,downloads); SeriesScreen offline(session,show,{},true,projection.seasons(show.id)); CHECK(offline.diagnosticSeasonsReady()&&offline.diagnosticSeasons().size()==(size_t)1&&offline.diagnosticSeasons()[0].id==cached.id);
}
static void testNewGridAndSchedule(){CHECK(moveMovieGrid(8,10,0,1)==8);CHECK(moveMovieGrid(9,10,0,-1)==9);CHECK(moveMovieGrid(8,10,1,0)==9);CHECK(clampMovieGridScroll(36,100,0)==1);MovieArtworkRange a=movieVisibleArtworkRange(0,36);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(0,37);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(1,37);CHECK(a.first==9&&a.lastExclusive==37);a=movieVisibleArtworkRange(4,100);CHECK(a.first==36&&a.lastExclusive==72);a=movieVisibleArtworkRange(9,100);CHECK(a.first==81&&a.lastExclusive==100);
    // A successful sync is fresh for fifteen minutes, and tab requests while
    // in flight coalesce instead of spawning another worker.
    LibrarySyncSchedule q; CHECK(q.request(0)); CHECK(!q.request(1)); CHECK(q.pending); CHECK(!q.complete(10,true)); CHECK(!q.request(10+LibrarySyncSchedule::FRESH_MS-1)); CHECK(q.request(10+LibrarySyncSchedule::FRESH_MS));
    // Failed cached refreshes wait before another normal-navigation attempt.
    LibrarySyncSchedule failed; CHECK(failed.request(0)); failed.complete(10,false); CHECK(!failed.request(11)); CHECK(failed.request(10+LibrarySyncSchedule::RETRY_DELAY_MS));
    // Shows progress is real, clamped and monotonic; poster work is not an input.
    ShowsSyncProgress zero{0,120}, mid{48,120}, done{120,120}, overflow{140,120}, regressed{12,120}; CHECK(zero.percent()==0); CHECK(mid.percent()==40); CHECK(done.percent()==100); CHECK(overflow.percent()==100); CHECK(regressed.percent(40)==40);
    // Cached content remains browseable and explicitly reports offline rather
    // than promoting a transient network failure to a fatal browsing error.
    CHECK_EQ(librarySyncStatus(0,true,true,false,true),"OFFLINE");
    CHECK_EQ(librarySyncStatus(1,true,false,true,true),"SYNCING...");
    CHECK_EQ(librarySyncStatus(2,true,false,false,true,mid,true),"SYNC 40%");
    CHECK_EQ(librarySyncStatus(2,true,false,false,true,done,false),"SYNCED");
}
static void testCacheRemoveNew(){std::string old=ImageCache::cacheDir();ImageCache::setCacheDir("/tmp/miyoofin-images/");unsigned char b[2]={1,2};CHECK(ImageCache::writeToCache("a",ImageType::Primary,"x",64,96,b,2));CHECK(ImageCache::writeToCache("b",ImageType::Primary,"x",64,96,b,2));CHECK(ImageCache::removeCached("a",ImageType::Primary,"x",64,96));CHECK(!ImageCache::isCached("a",ImageType::Primary,"x",64,96));CHECK(ImageCache::isCached("b",ImageType::Primary,"x",64,96));ImageCache::setCacheDir(old);}

static DownloadItem planItem(const std::string &id, std::uint64_t size, std::uint64_t done=0) {
    DownloadItem i; i.itemId=id; i.expectedSize=size; i.downloadedBytes=done; return i;
}

static void testDownloadsUiHelpers()
{
    std::printf("[test] Downloads UI helpers\n");
    CHECK(std::string(downloadStateLabel(DownloadState::Queued)) == "Queued");
    CHECK(std::string(downloadStateLabel(DownloadState::PausedForPlayback)) == "Paused for playback");
    CHECK(std::string(downloadStateLabel(DownloadState::NoSpace)) == "No space");
    DownloadItem episode; episode.itemType="episode"; episode.title="Slumber Party Panic"; episode.seasonNumber=1; episode.episodeNumber=1;
    CHECK(episodeDownloadLabel(episode) == "S01E01 Slumber Party Panic");
    episode.expectedSize=200; episode.downloadedBytes=87; CHECK(downloadPercent(episode)==43);
    DownloadItem hls; hls.hlsStorage=true; hls.hlsSegmentCount=4; hls.expectedSize=1000000; // storage estimate must not affect active percentage
    hls.hlsCompletedSegments=2; hls.downloadedBytes=1; CHECK(downloadPercent(hls)==50);
    hls.hlsCurrentSegmentBytes=25; hls.hlsCurrentSegmentSize=100; CHECK(downloadPercent(hls)==56); hls.hlsActivePercent=56;
    hls.hlsCurrentSegmentBytes=1; hls.hlsCurrentSegmentSize=0; CHECK(downloadPercent(hls)==56); // monotonic when a retry has no length
    hls.hlsCompletedSegments=4; hls.hlsActivePercent=99; CHECK(downloadPercent(hls)==99); // validation owns 100%
    hls.state=DownloadState::Complete; CHECK(downloadPercent(hls)==100);
    CHECK(downloadPrimaryControl(DownloadState::Complete)==DownloadPrimaryControl::Play);
    CHECK(downloadPrimaryControl(DownloadState::Downloading)==DownloadPrimaryControl::Pause);
    CHECK(downloadPrimaryControl(DownloadState::Paused)==DownloadPrimaryControl::Resume);
    CHECK(downloadPrimaryControl(DownloadState::WaitingForNetwork)==DownloadPrimaryControl::Retry);
    CHECK(downloadPrimaryControl(DownloadState::Unauthorized)==DownloadPrimaryControl::None);
    CHECK(clampDownloadSelection(4, 2)==1); CHECK(clampDownloadSelection(0, 0)==0);
    CHECK(clampDownloadScroll(6, 7, 0, 5)==2); CHECK(clampDownloadScroll(0, 0, 3, 5)==0);
    DownloadItem complete; complete.itemId="done"; complete.state=DownloadState::Complete;
    DownloadItem incomplete; incomplete.itemId="later"; incomplete.state=DownloadState::Queued;
    CHECK(downloadCanRemove(complete)); CHECK(downloadRemoveIsDelete(complete));
    CHECK(downloadCanRemove(incomplete)); CHECK(!downloadRemoveIsDelete(incomplete));
    CHECK(!downloadCanRemove(DownloadItem{}));
    CHECK(!downloadRemovalConfirmed("", incomplete));
    CHECK(!downloadRemovalConfirmed("other", incomplete));
    CHECK(downloadRemovalConfirmed("later", incomplete));
    std::printf("[test] Downloads UI helpers OK\n");
}

static void testDownloadHierarchy()
{
    std::printf("[test] download hierarchy\n");
    DownloadSnapshot snapshot;
    DownloadItem movie=planItem("movie",100,50); movie.itemType="movie"; movie.title="A Movie";
    DownloadItem a1=planItem("a1",100,100); a1.itemType="episode"; a1.title="Second"; a1.seriesId="a"; a1.seriesName="The Alpha Show"; a1.seasonId="a2"; a1.seasonName="Season 2"; a1.seasonNumber=2; a1.episodeNumber=2; a1.state=DownloadState::Complete;
    DownloadItem a0=planItem("a0",100,25); a0.itemType="episode"; a0.title="First"; a0.seriesId="a"; a0.seriesName="The Alpha Show"; a0.seasonId="a1"; a0.seasonName="Season 1"; a0.seasonNumber=1; a0.episodeNumber=1; a0.state=DownloadState::Downloading;
    DownloadItem b=planItem("b",200,200); b.itemType="episode"; b.title="Only"; b.seriesId="b"; b.seriesName="Breaking Bad"; b.seasonId="b1"; b.seasonNumber=1; b.episodeNumber=1; b.state=DownloadState::Complete;
    snapshot.items={movie,a1,a0,b};
    auto collapsed=buildDownloadHierarchy(snapshot,{});
    CHECK(collapsed.movies.size()==(size_t)1); CHECK(collapsed.shows.size()==(size_t)2); // movies separated; multiple series
    CHECK(collapsed.shows[0].title=="The Alpha Show"); // article-aware ordering
    CHECK(collapsed.shows[0].aggregate.episodes==2 && collapsed.shows[0].aggregate.active==1);
    CHECK(collapsed.shows[0].aggregate.progress==62); // (25 + complete 100) / 2
    std::set<std::string> expanded{"series:a","series:a/season:a1","series:a/season:a2"};
    auto open=buildDownloadHierarchy(snapshot,expanded);
    CHECK(open.visible.size()==(size_t)7); // movie, 2 series, 2 seasons, 2 episodes
    CHECK(open.shows[1].kind==DownloadHierarchyRowKind::Season && open.shows[1].title=="Season 1");
    CHECK(open.shows[2].kind==DownloadHierarchyRowKind::Episode && open.shows[2].item->itemId=="a0");
    CHECK(open.shows[3].kind==DownloadHierarchyRowKind::Season && open.shows[3].title=="Season 2");
    // Rebuild preserves expansion by stable IDs and selection by logical row.
    DownloadSnapshot refreshed=snapshot;
    auto after=buildDownloadHierarchy(refreshed,expanded);
    int selected=downloadHierarchySelection(after.visible,"series:a/season:a1",4);
    CHECK(after.shows[0].expanded && after.shows[1].expanded); CHECK(after.visible[selected].id=="series:a/season:a1");
    // A deleted selected leaf clamps safely; an empty category does not create a section.
    CHECK(downloadHierarchySelection(after.visible,"episode:deleted",99)==(int)after.visible.size()-1);
    DownloadSnapshot episodesOnly; episodesOnly.items={a0}; CHECK(buildDownloadHierarchy(episodesOnly,{}).movies.empty());
    DownloadSnapshot moviesOnly; moviesOnly.items={movie}; CHECK(buildDownloadHierarchy(moviesOnly,{}).shows.empty());
    CHECK(downloadPrimaryControl(a0.state)==DownloadPrimaryControl::Pause); // leaf controls remain unchanged
    // Y is handled by Downloads for hierarchy parents, so it cannot reach
    // HomeScreen's global ActionsMenu/logout path.
    DownloadHierarchyRow seriesRow; seriesRow.kind=DownloadHierarchyRowKind::Series;
    DownloadHierarchyRow seasonRow; seasonRow.kind=DownloadHierarchyRowKind::Season;
    CHECK(downloadHierarchyConsumesActionsMenu(seriesRow));
    CHECK(downloadHierarchyConsumesActionsMenu(seasonRow));
    // Parent Y plans only matching local DownloadManager IDs.  The plan is
    // later executed solely through DownloadManager::erase, never a server API.
    const DownloadHierarchyRow &alphaSeries=open.shows[0];
    const DownloadHierarchyRow &alphaSeason=open.shows[1];
    auto seasonDelete=downloadHierarchyBulkRemovalItemIds(alphaSeason,snapshot);
    CHECK(seasonDelete.size()==(size_t)1 && seasonDelete[0]=="a0");
    auto seriesDelete=downloadHierarchyBulkRemovalItemIds(alphaSeries,snapshot);
    CHECK(seriesDelete.size()==(size_t)2);
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"a0")!=seriesDelete.end());
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"a1")!=seriesDelete.end());
    CHECK(std::find(seriesDelete.begin(),seriesDelete.end(),"b")==seriesDelete.end());
    CHECK(downloadHierarchyBulkRemovalConfirmed(alphaSeason.id,alphaSeason));
    CHECK(!downloadHierarchyBulkRemovalConfirmed(alphaSeries.id,alphaSeason));
    DownloadSnapshot afterSeason=snapshot;
    afterSeason.items.erase(afterSeason.items.begin()+2);
    auto afterSeasonHierarchy=buildDownloadHierarchy(afterSeason,expanded);
    CHECK(std::none_of(afterSeasonHierarchy.shows.begin(),afterSeasonHierarchy.shows.end(),[](const DownloadHierarchyRow &row){ return row.id=="series:a/season:a1"; }));
    CHECK(std::any_of(afterSeasonHierarchy.shows.begin(),afterSeasonHierarchy.shows.end(),[](const DownloadHierarchyRow &row){ return row.id=="series:b"; }));
    DownloadSnapshot afterSeries=snapshot;
    afterSeries.items.erase(afterSeries.items.begin()+1,afterSeries.items.begin()+3);
    auto afterSeriesHierarchy=buildDownloadHierarchy(afterSeries,expanded);
    CHECK(afterSeriesHierarchy.shows.size()==(size_t)1 && afterSeriesHierarchy.shows[0].title=="Breaking Bad");
    std::printf("[test] download hierarchy OK\n");
}

static void testDownloadSourceReconciliation()
{
    std::printf("[test] download source reconciliation\n");
    DownloadItem item; item.itemId="item"; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; item.downloadedBytes=100; item.state=DownloadState::Complete;
    DownloadMediaSource source; source.id="source-a"; source.etag="etag-a"; source.size=100;
    CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::Complete);
    source.etag="etag-b"; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable&&item.sourceEtag=="etag-a");
    item.state=DownloadState::Complete; item.updateAvailable=false; source.etag="etag-a"; source.size=101; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable);
    item.state=DownloadState::Complete; item.updateAvailable=false; source.size=100; source.id="source-b"; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.state==DownloadState::UpdateAvailable);
    item.state=DownloadState::Complete; CHECK(!reconcileSource(item,SourceCheck::Missing)); CHECK(item.state==DownloadState::LocalOnly);
    CHECK(!reconcileSource(item,SourceCheck::Transient)); CHECK(item.state==DownloadState::LocalOnly); CHECK(!reconcileSource(item,SourceCheck::Unauthorized)); CHECK(item.state==DownloadState::LocalOnly);
    item.state=DownloadState::Paused; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; item.downloadedBytes=45; source.id="source-b"; source.etag="etag-b"; source.size=120;
    CHECK(reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.downloadedBytes==0&&item.expectedSize==120&&item.state==DownloadState::Queued);
    item.state=DownloadState::Complete; item.downloadedBytes=100; item.mediaSourceId="source-a"; item.sourceEtag="etag-a"; item.expectedSize=100; CHECK(!reconcileSource(item,SourceCheck::Same,&source)); CHECK(item.downloadedBytes==100&&item.expectedSize==100);
    std::string root="output/test/download-source-fixture-"+std::to_string((long long)getpid()); Session session; session.serverUrl="http://server"; session.userId="user"; session.accessToken="token"; session.deviceId="device";
    DownloadStore store(root); std::string scope=DownloadStore::scopeKey(session.serverUrl,session.userId); DownloadItem local; local.itemId="item"; local.expectedSize=4; local.chunkSize=4; local.downloadedBytes=4; local.state=DownloadState::UpdateAvailable; CHECK(store.saveManifest(scope,local)); CHECK(::mkdir((store.itemPath(scope,"item")+"/chunks").c_str(),0755)==0); FILE *f=std::fopen(store.chunkPath(scope,"item",0).c_str(),"wb"); CHECK(f!=nullptr); std::fwrite("data",1,4,f); std::fclose(f); CHECK(store.saveIndex(scope,{local}));
    DownloadManager d(session,root); MediaItem media; media.id="item"; CHECK(resolvePlayback(media,d,true)==PlaybackSource::Local); local.state=DownloadState::LocalOnly; CHECK(store.saveManifest(scope,local)); d.configure(session); CHECK(resolvePlayback(media,d,true)==PlaybackSource::Local);
    DownloadItem updated; updated.state=DownloadState::UpdateAvailable; CHECK(downloadIsLocal(local)); CHECK(downloadIsLocal(updated));
    DownloadItem partial; partial.itemId="partial"; partial.expectedSize=4; partial.chunkSize=4; CHECK(store.saveManifest(scope,partial)); CHECK(::mkdir((store.itemPath(scope,"partial")+"/chunks").c_str(),0755)==0); f=std::fopen(store.chunkPath(scope,"partial",0,true).c_str(),"wb"); CHECK(f!=nullptr); std::fwrite("xx",1,2,f); std::fclose(f); CHECK(store.removePartialBytes(scope,"partial")); CHECK(::access(store.chunkPath(scope,"partial",0,true).c_str(),F_OK)!=0);
    std::printf("[test] download source reconciliation OK\n");
}

static void testDownloadPlanBatchAccounting()
{
    // A manager with an invalid session never performs HTTP; makePlan is pure
    // with respect to its input and the manager's local queue.
    DownloadManager d({}, "output/test/download-plan-fixture-"+std::to_string((long long)getpid()));
    d.setPlaybackActive(true); // preserve fixture queue state; no transfer worker races
    // enqueue is called by the SDL confirmation handler.  With the worker
    // paused, no manifest or item directory may exist until that worker runs.
    DownloadItem deferred=planItem("deferred",100);
    d.enqueue(std::vector<DownloadItem>{deferred,planItem("deferred-two",100)});
    DownloadStore deferredStore("output/test/download-plan-fixture-"+std::to_string((long long)getpid()));
    CHECK(::access(deferredStore.manifestPath(d.scope(),"deferred").c_str(),F_OK)!=0);
    CHECK(::access(deferredStore.itemPath(d.scope(),"deferred").c_str(),F_OK)!=0);
    auto duplicate=d.makePlan({planItem("a",100),planItem("a",100),planItem("b",200)});
    CHECK(duplicate.items.size() == (size_t)2); // season / cross-season dedupe
    CHECK(duplicate.additionalRequiredBytes == (std::uint64_t)300);
    auto unknown=d.makePlan({planItem("unknown",0)});
    CHECK(!unknown.sizeKnown); CHECK(!unknown.error.empty());
    CHECK(!DownloadManager::acceptsPlanResult(7,8)); // stale planner generation is ignored
    CHECK(DownloadManager::acceptsPlanResult(8,8));
    std::vector<DownloadItem> large; for(int n=0;n<128;++n) large.push_back(planItem("episode"+std::to_string(n),1));
    CHECK(d.makePlan(large).items.size() == (size_t)128); // large series stays one plan
    d.enqueue(planItem("partial",100,40));
    auto partial=d.makePlan({planItem("partial",100)});
    CHECK(partial.alreadyPresentBytes == (std::uint64_t)40);
    CHECK(partial.additionalRequiredBytes == (std::uint64_t)0); // its remaining 60 is reserved once
    d.enqueue(planItem("complete",100,100));
    CHECK(d.makePlan({planItem("complete",100)}).additionalRequiredBytes == (std::uint64_t)0);
    auto queued=d.makePlan({planItem("partial",100),planItem("new",10)});
    CHECK(queued.additionalRequiredBytes == (std::uint64_t)10); // queued partial is not double-reserved
}

static void testHlsSizeEstimates()
{
    CHECK(EpisodeBrowserScreen::hasSeparateDownloadActions()); // Episode and Season controls coexist.
    std::uint64_t bytes=0;
    CHECK(estimateHlsBytes(22LL*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 224597536ULL);
    CHECK(estimateHlsBytes(45LL*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 459335536ULL);
    CHECK(estimateHlsBytes(2LL*60*60*HLS_TICKS_PER_SECOND,bytes)); CHECK(bytes == 1224785536ULL);
    CHECK(!estimateHlsBytes(0,bytes)); CHECK(estimateHlsBytes((std::numeric_limits<std::int64_t>::max)(),bytes)); CHECK(bytes > 100000000000000ULL);
    DownloadManager d({}, "output/test/hls-plan-fixture-"+std::to_string((long long)getpid())); d.setPlaybackActive(true);
    auto hls=[](const std::string&id,std::int64_t ticks){DownloadItem i;i.itemId=id;i.hlsStorage=true;i.runtimeTicks=ticks;i.expectedSize=9999999999ULL;return i;};
    auto season=d.makePlan({hls("one",22LL*60*HLS_TICKS_PER_SECOND),hls("two",45LL*60*HLS_TICKS_PER_SECOND)});
    CHECK(season.sizeKnown&&season.additionalRequiredBytes == 683933072ULL); // season total
    auto series=d.makePlan({hls("one",22LL*60*HLS_TICKS_PER_SECOND),hls("two",45LL*60*HLS_TICKS_PER_SECOND),hls("three",2LL*60*60*HLS_TICKS_PER_SECOND)});
    CHECK(series.sizeKnown&&series.additionalRequiredBytes == 1908718608ULL); // series total
    CHECK(!d.makePlan({hls("unknown",0)}).sizeKnown);
    // Cached metadata produces an HLS estimate before PlaybackInfo is fetched.
    DownloadItem metadataOnly=hls("metadata-only",22LL*60*HLS_TICKS_PER_SECOND);
    CHECK(d.makePlan({metadataOnly}).sizeKnown);
    DownloadItem complete=hls("actual",22LL*60*HLS_TICKS_PER_SECOND); complete.state=DownloadState::Complete; complete.downloadedBytes=12345;
    CHECK(displayDownloadBytes(complete)==12345);
    // Queue accounting switches from the conservative estimate to actual HLS
    // segment progress as soon as the active playlist is known.
    DownloadItem active=hls("active",22LL*60*HLS_TICKS_PER_SECOND); active.state=DownloadState::Downloading; active.hlsSegmentCount=4; active.hlsCompletedSegments=2; active.downloadedBytes=20ULL*1024*1024;
    CHECK(queueRemainingBytes(active)==20ULL*1024*1024);
    active.state=DownloadState::Complete;
    CHECK(queueRemainingBytes(active)==0);
    DownloadItem queued=hls("queued",22LL*60*HLS_TICKS_PER_SECOND); queued.state=DownloadState::Queued;
    CHECK(queueRemainingBytes(queued)==queued.expectedSize); // no playlist: retain reservation estimate
}

static void testHlsFailureClassification()
{
    std::printf("[test] recent transfer speed\n");
    RecentSpeedSample speed; std::uint64_t bytesPerSec=0;
    CHECK(!DownloadManager::updateRecentSpeed(speed,100,1000,bytesPerSec));
    CHECK(DownloadManager::updateRecentSpeed(speed,600,1500,bytesPerSec));
    CHECK(bytesPerSec==1000);
    // A short HLS segment gap retains the rolling rate.
    CHECK(!DownloadManager::updateRecentSpeed(speed,600,2900,bytesPerSec));
    CHECK(bytesPerSec==1000);
    CHECK(DownloadManager::updateRecentSpeed(speed,600,3001,bytesPerSec));
    CHECK(bytesPerSec==0);
    // A new download (a lower byte count) resets rather than carrying speed.
    CHECK(!DownloadManager::updateRecentSpeed(speed,0,3100,bytesPerSec));
    CHECK(bytesPerSec==0);
    std::printf("[test] recent transfer speed OK\n");

    CHECK(DownloadManager::hlsSegmentRetryable(520, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(502, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(503, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(504, CURLE_OK));
    CHECK(DownloadManager::hlsSegmentRetryable(0, CURLE_OPERATION_TIMEDOUT));
    CHECK(DownloadManager::hlsSegmentRetryable(0, CURLE_RECV_ERROR));
    CHECK(!DownloadManager::hlsSegmentRetryable(401, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(403, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(404, CURLE_OK));
    CHECK(!DownloadManager::hlsSegmentRetryable(0, CURLE_ABORTED_BY_CALLBACK));
    CHECK(DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, 1)); // a later attempt can succeed
    CHECK(DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, 4));
    CHECK(!DownloadManager::hlsSegmentShouldRetry(520, CURLE_OK, DownloadManager::HLS_SEGMENT_ATTEMPTS)); // exhausted
    CHECK(JellyfinApi::classifyHlsFailure(0, CURLE_OPERATION_TIMEDOUT) == JellyfinApi::HlsFailure::Timeout);
    CHECK(JellyfinApi::classifyHlsFailure(0, CURLE_COULDNT_CONNECT) == JellyfinApi::HlsFailure::Network);
    CHECK(JellyfinApi::classifyHlsFailure(401, CURLE_OK) == JellyfinApi::HlsFailure::Unauthorized);
    CHECK(JellyfinApi::classifyHlsFailure(403, CURLE_OK) == JellyfinApi::HlsFailure::Unauthorized);
    CHECK(JellyfinApi::classifyHlsFailure(500, CURLE_OK) == JellyfinApi::HlsFailure::Http);
    CHECK(JellyfinApi::classifyHlsFailure(404, CURLE_OK) == JellyfinApi::HlsFailure::Http);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_OPERATION_TIMEDOUT) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_COULDNT_CONNECT) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(0, CURLE_COULDNT_RESOLVE_HOST) == DownloadState::WaitingForNetwork);
    CHECK(DownloadManager::hlsSegmentFailureState(401, CURLE_OK) == DownloadState::Unauthorized);
    CHECK(DownloadManager::hlsSegmentFailureState(500, CURLE_OK) == DownloadState::Failed);
    CHECK(DownloadManager::hlsSegmentFailureState(404, CURLE_OK) == DownloadState::Failed);
}

// --- Playback progress display tests ---

static void testPlaybackPercentFromTicks()
{
    std::printf("[test] playbackPercent from ticks (position/runtime)\n");
    MediaItem item;
    item.runTimeTicks = 10000000LL * 60 * 60;   // 1 hour
    item.playbackPositionTicks = 10000000LL * 60 * 37; // 37 minutes
    item.progress = 0.5f;
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(62)); // 37/60 ≈ 62%
    std::printf("[test] playbackPercent from ticks (position/runtime) OK\n");
}

static void testPlaybackPercentFallbackToProgress()
{
    std::printf("[test] playbackPercent fallback to progress field\n");
    MediaItem item;
    item.runTimeTicks = 0;          // no runtime
    item.playbackPositionTicks = 0;
    item.progress = 0.455f;         // 45.5% from server
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(46)); // 45.5 rounds to 46
    std::printf("[test] playbackPercent fallback to progress field OK\n");
}

static void testPlaybackPercentClamping()
{
    std::printf("[test] playbackPercent clamped to 0-100\n");
    {
        MediaItem item;
        item.runTimeTicks = 10000000LL * 60;
        item.playbackPositionTicks = 10000000LL * 90; // 90 min in 60-min runtime
        item.progress = 0.0f;
        item.played = false;
        int pct = playbackPercent(item);
        CHECK_EQ(std::to_string(pct), std::to_string(100));
    }
    {
        MediaItem item;
        item.runTimeTicks = 0;
        item.playbackPositionTicks = 0;
        item.progress = 1.5f; // > 1.0 (shouldn't happen but test guard)
        item.played = false;
        int pct = playbackPercent(item);
        CHECK_EQ(std::to_string(pct), std::to_string(100));
    }
    std::printf("[test] playbackPercent clamped to 0-100 OK\n");
}

static void testPlaybackPercentZeroRuntime()
{
    std::printf("[test] playbackPercent zero runtime returns 0\n");
    MediaItem item;
    item.runTimeTicks = 0;
    item.playbackPositionTicks = 0;
    item.progress = 0.0f;
    item.played = false;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(0));
    std::printf("[test] playbackPercent zero runtime returns 0 OK\n");
}

static void testPlaybackPercentCompletedItem()
{
    std::printf("[test] playbackPercent completed item uses ticks\n");
    MediaItem item;
    item.runTimeTicks = 10000000LL * 60 * 45; // 45 min
    item.playbackPositionTicks = 10000000LL * 60 * 45; // fully watched
    item.progress = 1.0f;
    item.played = true;
    int pct = playbackPercent(item);
    CHECK_EQ(std::to_string(pct), std::to_string(100));
    std::printf("[test] playbackPercent completed item uses ticks OK\n");
}

static void testFormatPlaybackTimeBasic()
{
    std::printf("[test] formatPlaybackTime basic minutes\n");
    // 30 min position in 60 min runtime
    std::string s = formatPlaybackTime(10000000LL * 60 * 30,
                                       10000000LL * 60 * 60);
    CHECK(s.find("30m watched") != std::string::npos);
    CHECK(s.find("30m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime basic minutes OK\n");
}

static void testFormatPlaybackTimeHours()
{
    std::printf("[test] formatPlaybackTime hours and minutes\n");
    // 1h24m position in 2h runtime
    long long pos = 10000000LL * (60*60 + 24*60); // 1h 24m
    long long tot = 10000000LL * 60 * 120;          // 2h
    std::string s = formatPlaybackTime(pos, tot);
    CHECK(s.find("1h 24m watched") != std::string::npos);
    CHECK(s.find("36m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime hours and minutes OK\n");
}

static void testFormatPlaybackTimeZeroRuntime()
{
    std::printf("[test] formatPlaybackTime zero runtime returns empty\n");
    std::string s = formatPlaybackTime(50000000LL, 0);
    CHECK(s.empty());
    std::printf("[test] formatPlaybackTime zero runtime returns empty OK\n");
}

static void testFormatPlaybackTimeClampedPosition()
{
    std::printf("[test] formatPlaybackTime position clamped to runtime\n");
    // position exceeds runtime — should clamp
    std::string s = formatPlaybackTime(10000000LL * 60 * 90,
                                       10000000LL * 60 * 60);
    CHECK(s.find("1h 0m watched") != std::string::npos);
    CHECK(s.find("0m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime position clamped to runtime OK\n");
}

static void testFormatPlaybackTimeZeroPosition()
{
    std::printf("[test] formatPlaybackTime zero position\n");
    std::string s = formatPlaybackTime(0, 10000000LL * 60 * 90);
    CHECK(s.find("0m watched") != std::string::npos);
    CHECK(s.find("1h 30m remaining") != std::string::npos);
    std::printf("[test] formatPlaybackTime zero position OK\n");
}

// -------------------------------------------------------------------
// Clock check tests — Issue #1: friendly error when device clock breaks HTTPS
// Tests exercise the shared shouldShowClockError() decision helper
// directly, with no conceptual/indirect assertions.
// -------------------------------------------------------------------

static void testShouldShowClockErrorTrue()
{
    std::printf("[test] shouldShowClockError: peer-fail + 1970 -> true\n");
    // Peer verification failed AND clock is 1970 — must fire.
    CHECK(shouldShowClockError(true, std::time_t(0)));
    CHECK(shouldShowClockError(true, std::time_t(591913)));
    // One second before the 2020 boundary is still invalid.
    CHECK(shouldShowClockError(true, std::time_t(1577836799L)));
    std::printf("[test] shouldShowClockError: peer-fail + 1970 -> true OK\n");
}

static void testShouldShowClockErrorFalseModernEpoch()
{
    std::printf("[test] shouldShowClockError: peer-fail + modern epoch -> false\n");
    // Peer verification failed but clock is fine — must NOT fire.
    CHECK(!shouldShowClockError(true, std::time_t(1577836800L)));  // exactly 2020-01-01
    CHECK(!shouldShowClockError(true, std::time_t(1750000000L)));  // 2025
    std::printf("[test] shouldShowClockError: peer-fail + modern epoch -> false OK\n");
}

static void testShouldShowClockErrorFalseUnrelatedFailure()
{
    std::printf("[test] shouldShowClockError: unrelated failure + 1970 -> false\n");
    // Clock is wrong but the curl error is NOT peer verification — must NOT fire.
    CHECK(!shouldShowClockError(false, std::time_t(0)));
    CHECK(!shouldShowClockError(false, std::time_t(591913)));
    std::printf("[test] shouldShowClockError: unrelated failure + 1970 -> false OK\n");
}

static void testClockMessageFormat()
{
    std::printf("[test] kClockErrorMessage: one-line, short, contains OnionOS path\n");
    std::string msg = kClockErrorMessage;
    // Must be short enough to fit in wrapCols=75 without wrapping.
    CHECK(msg.size() <= 75);
    // Must contain no newline.
    CHECK(msg.find('\n') == std::string::npos);
    // Must contain the OnionOS navigation path.
    CHECK(msg.find("OnionOS") != std::string::npos);
    CHECK(msg.find("Apps") != std::string::npos);
    CHECK(msg.find("Tweaks") != std::string::npos);
    CHECK(msg.find("Date/time") != std::string::npos);
    std::printf("[test] kClockErrorMessage: one-line, short, contains OnionOS path OK\n");
}

int main()
{
    testRouteRequest();
    testServerEntryKeyboardCaps();
    testSettingsAddressEntryCancel();
    testLoginKeyboardCaps();
    testOnScreenKeyboardGrid();
    testOnScreenKeyboardSpace();
    testServerEntrySpace();
    testLoginUsernameSpace();
    testLoginPasswordSpace();
    testSharedKeyboardLayoutConsistency();
    testKeyboardVerticalNavActionRow();
    testUiDiagnostics();
    std::printf("\n--- Movie title organization tests ---\n");
    testMovieOrganizationalTitles();
    testMovieAlphabetOrganization();
    testMovieOrganizationalSort();
    testMovieAlphabetFocus();
    testShowsPresentation();

    std::printf("\n--- local-first cache/grid tests ---\n");
    testLibraryCacheNew(); testLibraryCacheV3SaveLoad(); testLibraryCacheV2BackCompat(); testLibraryCacheV1BackCompat(); testLibraryCacheUnknownVersion(); testLibraryCacheFetchDecision(); testSyncState(); testOfflineCatalog(); testOfflineLibraryProjection(); testSettingsRowActions(); testLanServerAddressClassificationAndSettingsLayout(); testSeriesCachedSeasonHandoff(); testSeasonPosterScheduling(); testNewGridAndSchedule(); testCacheRemoveNew();
    std::printf("MiyooFin Checkpoint B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a tests\n");
    std::printf("==============================================================================\n\n");

    // B3 tests
    testNormaliseUrl();
    testSession();
    testSessionEmpty();
    testSessionBackwardCompatibility();
    testSessionAtomicNoTmpResidue();
    testSessionAtomicReplacePreservesNewContent();
    testSystemInfoParsing();
    testLocalServerIdentityVerification();
    testDeviceIdentity();
    testDeviceIdentityLoadOrCreate();
    testAuthTypes();

    // B4 tests
    testMediaItemDefaults();
    testJsonStringField();
    testJsonIntFloatBool();
    testJsonExtractArray();
    testJsonToMediaItem();
    testBuildTabs();
    testContinueWatchingRowRefresh();
    testLatestItemsDirectArray();
    testBuildLatestUrl();
    testBuildLibraryItemsUrl();
    testUnicodeEscapeDecoding();
    testBitmapFontMapCodePoint();
    testBitmapFontMapCodePointLatinAccents();
    testAmpersandAndJsonEscape();
    testGenreUnicodeEscapeDecoding();
    testBitmapFontTruncateUtf8();

    // B5a tests — Artwork infrastructure
    std::printf("\n--- B5a artwork infrastructure tests ---\n");
    testBuildImageUrlPrimary();
    testBuildImageUrlThumb();
    testImageTypeAndCacheKeys();
    testCacheFilename();
    testCacheWriteRead();
    testJpegDecodeValid();
    testJpegDecodeInvalid();
    testBinaryHttpResponse();

    // B5b tests — Selected artwork loading
    std::printf("\n--- B5b selected artwork tests ---\n");
    testNoPrimaryTagNoArtwork();
    testArtworkIdentityKey();
    testArtworkLoadGuard();
    testCachedJpegDecodeRoundtrip();
    testFailedLoadLeavesEmpty();
    testArtworkUrlDimensions();

    // B5c1 tests — Per-type artwork box dimensions
    std::printf("\n--- B5c1 per-type artwork box tests ---\n");
    testMovieArtworkBox();
    testShowArtworkBox();
    testEpisodeArtworkBox();
    testOtherArtworkBox();
    testMetadataXFollowsBoxWidth();

    // B5d1 tests — Row card geometry and scrolling
    std::printf("\n--- B5d1 row card geometry + scrolling tests ---\n");
    testMovieRowCard();
    testShowRowCard();
    testEpisodeRowCard();
    testMixedWidthPositions();
    testScrollRightKeepsVisible();
    testScrollLeftDecreases();
    testScrollNeverNegative();
    testScrollIsPixelNotIndex();

    // B5d2a tests — Row artwork loading state
    std::printf("\n--- B5d2a row artwork loading state tests ---\n");
    testMovieRowKeyPrimary();
    testEpisodeRowKeyPrimary();
    testNoPrimaryTagEmptyKey();
    testSameKeyNotLoadedTwice();
    testAllVisibleCandidatesScheduledPerCycle();

    // B5e1a tests — Season parsing groundwork
    std::printf("\n--- B5e1a season parsing tests ---\n");
    testSeasonIndexNumber();
    testSeasonTypeNormalization();

    // B5e2a tests — Episode parsing groundwork
    std::printf("\n--- B5e2a episode parsing tests ---\n");
    testEpisodeJsonParsing();
    testEpisodeTypeNormalization();
    testTicksToMinutes();
    testEpisodeDefaults();

    // B5e3b tests — Initial episode focus
    std::printf("\n--- B5e3b initial episode focus tests ---\n");
    testFindEpisodeIndexFound();
    testFindEpisodeIndexNotFound();
    testFindEpisodeIndexEmpty();
    testFindEpisodeIndexEmptyList();

    // B5g1b tests — bounded predictive episode thumbnail prefetch
    std::printf("\n--- B5g1b bounded prefetch scheduler tests ---\n");
    testEpisodePrefetchScheduler();
    testEpisodeArtworkPreemption();
    testEpisodePrefetchPlaybackResume();

    // B5f2 tests — Playback request
    std::printf("\n--- B5f2 playback request tests ---\n");
    testPlaybackRequestMovie();
    testPlaybackRequestEpisode();
    testPlaybackRequestEmptyId();
    testPlaybackRequestEmptyType();
    testPlaybackRequestRemove();
    testPlaybackResultParsing();
    testPlaybackResultDelay();
    testOfflinePlaybackJournal();

    std::printf("\n--- Download manager state tests ---\n");
    testHlsDownloadStore();
    testDownloadRestartPersistence();
    testDownloadInterruptStates();
    testDownloadPlanBatchAccounting();
    testHlsSizeEstimates(); testHlsFailureClassification();
    testDownloadsUiHelpers();
    testDownloadHierarchy();
    testDownloadSourceReconciliation();

    // B5f3a tests — In-process external playback handoff
    std::printf("\n--- B5f3a external playback handoff tests ---\n");
    testExternalPlaybackFlagInitial();
    testExternalPlaybackFlagSetConsume();
    testExternalPlaybackFlagMultipleSet();
    testPlaybackRequestStillWorks();
    testScreenStackPreservedDuringExternalPlayback();
    testScreenRetirementDoesNotBlockPop();
    testMovieDetailsOpensBeforeArtworkPreparation();
    testMovieDetailsUsesGridArtworkImmediately();

    // Central D-pad hold-to-repeat input timing
    std::printf("\n--- D-pad hold-to-repeat tests ---\n");
    testDpadHoldRepeatTiming();

    // Playback progress display helpers
    std::printf("\n--- Playback progress display tests ---\n");
    testPlaybackPercentFromTicks();
    testPlaybackPercentFallbackToProgress();
    testPlaybackPercentClamping();
    testPlaybackPercentZeroRuntime();
    testPlaybackPercentCompletedItem();
    testFormatPlaybackTimeBasic();
    testFormatPlaybackTimeHours();
    testFormatPlaybackTimeZeroRuntime();
    testFormatPlaybackTimeClampedPosition();
    testFormatPlaybackTimeZeroPosition();

    // Issue #1: Clock check tests — friendly HTTPS error when system clock is wrong
    std::printf("\n--- Clock check tests (Issue #1) ---\n");
    testShouldShowClockErrorTrue();
    testShouldShowClockErrorFalseModernEpoch();
    testShouldShowClockErrorFalseUnrelatedFailure();
    testClockMessageFormat();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a+progress tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
