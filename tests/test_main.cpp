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
#include "../src/image/ImageDecoder.hpp"
#include "../src/cache/ImageCache.hpp"
#include "../src/cache/LibraryCache.hpp"
#include "../src/cache/OfflineCatalog.hpp"
#include "../src/net/HttpClient.hpp"
#include "../src/ui/ArtworkLayout.hpp"
#include "../src/ui/MovieTitle.hpp"
#include "../src/ui/ShowsBrowser.hpp"
#include "../src/ui/screens/EpisodeBrowserScreen.hpp"
#include "../src/app/ScreenStack.hpp"
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
static void testNormaliseUrl()
{
    std::printf("[test] JellyfinApi::normaliseUrl\n");

    CHECK(JellyfinApi::normaliseUrl("http://192.168.1.50:8096")
          == "http://192.168.1.50:8096");
    CHECK(JellyfinApi::normaliseUrl("192.168.1.50:8096")
          == "http://192.168.1.50:8096");
    CHECK(JellyfinApi::normaliseUrl("http://jellyfin.local/")
          == "http://jellyfin.local");
    CHECK(JellyfinApi::normaliseUrl("https://media.example.com")
          == "https://media.example.com");
    CHECK(JellyfinApi::normaliseUrl("   http://myserver.com  ")
          == "http://myserver.com");
}

// -------------------------------------------------------------------
// Test 2: Session save/load/clear roundtrip
// -------------------------------------------------------------------
static void testSession()
{
    std::printf("[test] Session save/load\n");

    const char *tmpPath = "test_session.txt";
    std::remove(tmpPath);

    Session s1;
    s1.serverUrl   = "http://server:8096";
    s1.accessToken = "abc123token";
    s1.userId      = "user-42";
    s1.userName    = "testuser";
    s1.deviceId    = "dev-001";

    CHECK(s1.saveTo(tmpPath));

    Session s2 = Session::loadFrom(tmpPath);
    CHECK(s2.valid());
    CHECK_EQ(s2.serverUrl,   "http://server:8096");
    CHECK_EQ(s2.accessToken, "abc123token");
    CHECK_EQ(s2.userId,      "user-42");
    CHECK_EQ(s2.userName,    "testuser");
    CHECK_EQ(s2.deviceId,    "dev-001");

    s2.clear();
    CHECK(!s2.valid());
    CHECK(s2.serverUrl.empty());
    CHECK(s2.accessToken.empty());

    std::remove(tmpPath);
    std::printf("[test] Session save/load OK\n");
}

// -------------------------------------------------------------------
// Test 3: Session empty/missing file
// -------------------------------------------------------------------
static void testSessionEmpty()
{
    std::printf("[test] Session empty/missing\n");

    Session s = Session::loadFrom("nonexistent_session.txt");
    CHECK(!s.valid());
    CHECK(s.serverUrl.empty());

    const char *tmpPath = "test_empty_session.txt";
    std::remove(tmpPath);
    {
        FILE *f = std::fopen(tmpPath, "w");
        if (f) std::fclose(f);
    }
    s = Session::loadFrom(tmpPath);
    CHECK(!s.valid());
    std::remove(tmpPath);

    std::printf("[test] Session empty/missing OK\n");
}

// -------------------------------------------------------------------
// Test 4: DeviceIdentity UUID generation
// -------------------------------------------------------------------
static void testDeviceIdentity()
{
    std::printf("[test] DeviceIdentity UUID generation\n");

    std::string uuid = DeviceIdentity::generateUuidV4();
    std::printf("  Generated UUID: %s\n", uuid.c_str());

    CHECK(uuid.size() == 36);
    CHECK(uuid[8]  == '-');
    CHECK(uuid[13] == '-');
    CHECK(uuid[18] == '-');
    CHECK(uuid[23] == '-');
    CHECK(uuid[14] == '4');  // version 4

    char variant = uuid[19];
    CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');

    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = uuid[i];
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }

    std::string uuid2 = DeviceIdentity::generateUuidV4();
    CHECK(uuid != uuid2);

    std::printf("[test] DeviceIdentity UUID generation OK\n");
}

// -------------------------------------------------------------------
// Test 5: DeviceIdentity load-or-create
// -------------------------------------------------------------------
static void testDeviceIdentityLoadOrCreate()
{
    std::printf("[test] DeviceIdentity load-or-create\n");

    const char *tmpPath = "test_device.txt";
    std::remove(tmpPath);

    std::string id1 = DeviceIdentity::loadOrCreate(tmpPath);
    CHECK(!id1.empty());
    CHECK(id1.size() == 36);

    std::string id2 = DeviceIdentity::loadOrCreate(tmpPath);
    CHECK_EQ(id1, id2);

    std::remove(tmpPath);
    std::printf("[test] DeviceIdentity load-or-create OK\n");
}

// -------------------------------------------------------------------
// Test 6: AuthResult, AuthError, and version constants
// -------------------------------------------------------------------
static void testAuthTypes()
{
    std::printf("[test] AuthResult/AuthError types\n");

    AuthResult r;
    CHECK(r.accessToken.empty());
    CHECK(r.userId.empty());
    CHECK(r.userName.empty());

    r.accessToken = "tok";
    r.userId = "uid";
    r.userName = "uname";
    CHECK(r.accessToken == "tok");
    CHECK(r.userId == "uid");
    CHECK(r.userName == "uname");

    CHECK(AuthError::None == AuthError::None);
    CHECK(AuthError::Network != AuthError::InvalidCredentials);
    CHECK(AuthError::InvalidCredentials != AuthError::Unauthorized);

    CHECK(std::string(APP_NAME) == "MiyooFin");
    CHECK(std::string(DEVICE_NAME) == "Miyoo Mini Plus");
    CHECK(std::string(VERSION_STR) == "0.1.0");

    std::printf("[test] AuthResult/AuthError types OK\n");
}

// ===================================================================
// Checkpoint B4 tests
// ===================================================================

static void testMediaItemDefaults()
{
    std::printf("[test] MediaItem defaults\n");
    MediaItem item;
    CHECK(item.id.empty());
    CHECK(item.year == 0);
    CHECK(item.genres.empty());
    CHECK(item.played == false);
    CHECK(item.progress == 0.0f);
    CHECK(item.imageTags.empty());
    CHECK(item.artR == 128);
    std::printf("[test] MediaItem defaults OK\n");
}

static void testJsonStringField()
{
    std::printf("[test] jsonStringField\n");
    std::string obj = R"({"Id":"abc123","Name":"Test","Type":"Movie"})";
    CHECK(JellyfinApi::jsonStringField(obj, "Id") == "abc123");
    CHECK(JellyfinApi::jsonStringField(obj, "Name") == "Test");
    CHECK(JellyfinApi::jsonStringField(obj, "Missing").empty());
    std::printf("[test] jsonStringField OK\n");
}

static void testJsonIntFloatBool()
{
    std::printf("[test] jsonInt/Float/Bool\n");
    std::string obj = R"({"Y":2021,"R":7.5,"QR":"8.3","P":true,"F":false,"N":null})";
    CHECK(JellyfinApi::jsonIntField(obj, "Y") == 2021);
    CHECK(JellyfinApi::jsonIntField(obj, "N") == 0);
    CHECK(JellyfinApi::jsonIntField(obj, "X") == 0);
    float r = JellyfinApi::jsonFloatField(obj, "R");
    CHECK(r > 7.4f && r < 7.6f);
    float qr = JellyfinApi::jsonFloatField(obj, "QR");
    CHECK(qr > 8.2f && qr < 8.4f);
    CHECK(JellyfinApi::jsonBoolField(obj, "P") == true);
    CHECK(JellyfinApi::jsonBoolField(obj, "F") == false);
    CHECK(JellyfinApi::jsonBoolField(obj, "X") == false);
    std::printf("[test] jsonInt/Float/Bool OK\n");
}

static void testJsonExtractArray()
{
    std::printf("[test] jsonExtractArray\n");
    std::string j = R"({"Items":[{"Id":"a"},{"Id":"b"}],"Count":2})";
    auto items = JellyfinApi::jsonExtractArray(j, "Items");
    CHECK(items.size() == 2);
    CHECK(JellyfinApi::jsonStringField(items[0], "Id") == "a");
    CHECK(JellyfinApi::jsonStringField(items[1], "Id") == "b");
    CHECK(JellyfinApi::jsonExtractArray(j, "Missing").empty());
    CHECK(JellyfinApi::jsonExtractArray(R"({"Items":[]})", "Items").empty());
    std::printf("[test] jsonExtractArray OK\n");
}

static void testJsonToMediaItem()
{
    std::printf("[test] jsonToMediaItem\n");
    std::string j = R"({"Id":"i1","Name":"Test Movie","Type":"Movie",
        "ProductionYear":2021,"Overview":"A movie.","CommunityRating":7.5,
        "Genres":["Action","Sci-Fi"],
        "ImageTags":{"Primary":"tag1"},
        "UserData":{"Played":false,"PlayedPercentage":45.5,
        "PlaybackPositionTicks":18822664360}})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.id, "i1");
    CHECK_EQ(item.title, "Test Movie");
    CHECK_EQ(item.type, "movie");
    CHECK(item.year == 2021);
    CHECK(item.rating > 7.4f && item.rating < 7.6f);
    CHECK_EQ(item.overview, "A movie.");
    CHECK(item.genres.size() == 2);
    CHECK_EQ(item.genre, "Action");
    CHECK(item.played == false);
    CHECK(item.progress > 0.44f && item.progress < 0.46f);
    CHECK(item.playbackPositionTicks == 18822664360LL);
    CHECK(item.imageTags.size() == 1);
    CHECK_EQ(item.imageTags.at("Primary"), "tag1");

    // Series type normalization
    std::string j2 = R"({"Id":"s1","Name":"Show","Type":"Series"})";
    CHECK_EQ(JellyfinApi::jsonToMediaItem(j2).type, "show");

    // Minimal fields
    std::string j3 = R"({"Id":"m1","Name":"Min"})";
    auto m = JellyfinApi::jsonToMediaItem(j3);
    CHECK(m.year == 0); CHECK(m.genres.empty());
    CHECK(m.playbackPositionTicks == 0);
    CHECK(m.imageTags.empty());

    auto nullPosition = JellyfinApi::jsonToMediaItem(
        R"({"UserData":{"PlaybackPositionTicks":null}})");
    CHECK(nullPosition.playbackPositionTicks == 0);

    auto malformedPosition = JellyfinApi::jsonToMediaItem(
        R"({"UserData":{"PlaybackPositionTicks":not-a-number}})");
    CHECK(malformedPosition.playbackPositionTicks == 0);

    auto largePosition = JellyfinApi::jsonToMediaItem(
        R"({"UserData":{"PlaybackPositionTicks":9223372036854775806}})");
    CHECK(largePosition.playbackPositionTicks == 9223372036854775806LL);
    std::printf("[test] jsonToMediaItem OK\n");
}

static void testBuildTabs()
{
    std::printf("[test] buildTabs\n");
    std::vector<LibraryView> views = {{"v1","Movies","movies"},{"v2","TV","tvshows"}};
    std::vector<MediaItem> cw = {{"c1","CW Movie","",2020,7.0f,"Action","movie"}};
    std::vector<MediaItem> ra = {{"r1","New Movie","",2023,6.5f,"Comedy","movie"}};
    std::vector<std::pair<std::string, std::vector<MediaItem>>> mbv =
        {{"Movies", {{"m1","M1","",2020,7.0f,"A","movie"},
                     {"m2","M2","",2021,8.0f,"D","movie"}}}};
    std::vector<std::pair<std::string, std::vector<MediaItem>>> sbv =
        {{"TV", {{"s1","S1","",2019,9.0f,"S","show"}}}};

    auto tabs = JellyfinApi::buildTabs(views, cw, ra, mbv, sbv);
    CHECK(tabs.size() == 5);
    CHECK_EQ(tabs[0].name, "Home");
    CHECK_EQ(tabs[1].name, "Movies");
    CHECK_EQ(tabs[2].name, "Shows");
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK(tabs[0].rows[0].items.size() == 1);
    CHECK(tabs[1].rows[0].items.size() == 2);
    CHECK(tabs[2].rows[0].items.size() == 1);
    CHECK(tabs[3].rows[0].items.empty());  // Search placeholder
    CHECK(tabs[4].rows[0].items.empty());  // Downloads placeholder

    // Empty case
    std::vector<MediaItem> e;
    std::vector<std::pair<std::string, std::vector<MediaItem>>> ep;
    auto t2 = JellyfinApi::buildTabs({}, e, e, ep, ep);
    CHECK(t2.size() == 5);
    CHECK(t2[0].rows.size() == 1);
    CHECK(t2[0].rows[0].items.empty());
    std::printf("[test] buildTabs OK\n");
}

static void testContinueWatchingRowRefresh()
{
    std::printf("[test] Continue Watching row refresh\n");
    MediaItem oldItem; oldItem.id = "old";
    MediaItem newItem; newItem.id = "new";
    MediaItem recentItem; recentItem.id = "recent";

    std::vector<TabData> tabs = {{"Home", {
        {"Continue Watching", {oldItem}}, {"Recently Added", {recentItem}}
    }}};
    HomeScreen::updateContinueWatchingRow(tabs, {newItem});
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[0].items[0].id, "new");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");

    tabs = {{"Home", {{"Recently Added", {recentItem}}}}};
    HomeScreen::updateContinueWatchingRow(tabs, {newItem});
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");

    HomeScreen::updateContinueWatchingRow(tabs, {});
    CHECK(tabs[0].rows.size() == 1);
    CHECK_EQ(tabs[0].rows[0].label, "Recently Added");

    tabs = {{"Home", {{"Continue Watching", {oldItem}}}}};
    HomeScreen::updateContinueWatchingRow(tabs, {});
    CHECK(tabs[0].rows.size() == 1);
    CHECK(tabs[0].rows[0].label.empty());
    CHECK(tabs[0].rows[0].items.empty());

    HomeScreen::updateContinueWatchingRow(tabs, {newItem});
    CHECK(tabs[0].rows.size() == 1);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    std::printf("[test] Continue Watching row refresh OK\n");
}

// -------------------------------------------------------------------
// Test: Items/Latest direct-array parsing (BUG 1 regression)
// The /Items/Latest endpoint returns a bare JSON array, not wrapped
// in {"Items":[...]}. Verify multi-item parsing works.
// -------------------------------------------------------------------
static void testLatestItemsDirectArray()
{
    std::printf("[test] Latest items (direct array, 3 items)\n");

    // jsonExtractArray with "Items" on a direct array should return empty
    std::string directArr = "["
        "{\"Id\":\"a1\",\"Name\":\"Alpha\",\"Type\":\"Movie\"},"
        "{\"Id\":\"b2\",\"Name\":\"Beta\",\"Type\":\"Series\"},"
        "{\"Id\":\"c3\",\"Name\":\"Gamma\",\"Type\":\"Movie\"}"
        "]";
    auto empty = JellyfinApi::jsonExtractArray(directArr, "Items");
    CHECK(empty.empty());

    // Verify each item individually parses correctly
    std::string item1 = "{\"Id\":\"a1\",\"Name\":\"Alpha\",\"Type\":\"Movie\","
        "\"Overview\":\"First\",\"ProductionYear\":2020,"
        "\"CommunityRating\":8.0,\"Genres\":[\"Action\"],"
        "\"ImageTags\":{\"Primary\":\"t1\"},"
        "\"UserData\":{\"Played\":false,\"PlayedPercentage\":0}}";
    std::string item2 = "{\"Id\":\"b2\",\"Name\":\"Beta\",\"Type\":\"Series\","
        "\"Overview\":\"Second\",\"ProductionYear\":2021,"
        "\"CommunityRating\":7.5,\"Genres\":[\"Drama\",\"Comedy\"],"
        "\"ImageTags\":{\"Primary\":\"t2\"},"
        "\"UserData\":{\"Played\":true,\"PlayedPercentage\":100}}";
    std::string item3 = "{\"Id\":\"c3\",\"Name\":\"Gamma\",\"Type\":\"Movie\","
        "\"Overview\":\"Third\",\"ProductionYear\":2022,"
        "\"CommunityRating\":6.5,\"Genres\":[\"Sci-Fi\"],"
        "\"ImageTags\":{},"
        "\"UserData\":{\"Played\":false,\"PlayedPercentage\":45.5}}";

    auto m1 = JellyfinApi::jsonToMediaItem(item1);
    CHECK_EQ(m1.id, "a1");
    CHECK_EQ(m1.title, "Alpha");
    CHECK_EQ(m1.type, "movie");

    auto m2 = JellyfinApi::jsonToMediaItem(item2);
    CHECK_EQ(m2.id, "b2");
    CHECK_EQ(m2.title, "Beta");
    CHECK_EQ(m2.type, "show");
    CHECK(m2.played == true);

    auto m3 = JellyfinApi::jsonToMediaItem(item3);
    CHECK_EQ(m3.id, "c3");
    CHECK_EQ(m3.title, "Gamma");

    // Also verify wrapped {"Items":[...]} format works
    std::string wrapped = "{\"Items\":[" + item1 + "," + item2 + "," + item3 + "]}";
    auto items = JellyfinApi::jsonExtractArray(wrapped, "Items");
    CHECK(items.size() == 3);
    CHECK_EQ(JellyfinApi::jsonStringField(items[0], "Id"), "a1");
    CHECK_EQ(JellyfinApi::jsonStringField(items[1], "Id"), "b2");
    CHECK_EQ(JellyfinApi::jsonStringField(items[2], "Id"), "c3");

    std::printf("[test] Latest items (direct array) OK\n");
}

// -------------------------------------------------------------------
// Test: JSON Unicode escape decoding (BUG 2 regression)
// \u0022 -> ", \u00B2 -> UTF-8 for ², surrogate pairs
// -------------------------------------------------------------------
static void testUnicodeEscapeDecoding()
{
    std::printf("[test] Unicode escape decoding\n");

    // \u0022 should decode to "
    std::string s1 = R"({"Text":"hello \u0022world\u0022"})";
    CHECK_EQ(JellyfinApi::jsonStringField(s1, "Text"), "hello \"world\"");

    // \u00B2 should decode to UTF-8 for ²
    std::string s2 = R"({"Name":"m\u00B2"})";
    auto decoded = JellyfinApi::jsonStringField(s2, "Name");
    // Expected: 'm' + UTF-8 for U+00B2 (0xC2 0xB2) = 3 bytes
    CHECK_EQ(decoded, std::string("m\xC2\xB2"));

    // Surrogate pair: \uD83D\uDE00 -> U+1F600
    std::string s3 = R"({"Emoji":"test \uD83D\uDE00 end"})";
    auto s3dec = JellyfinApi::jsonStringField(s3, "Emoji");
    CHECK_EQ(s3dec, std::string("test \xF0\x9F\x98\x80 end"));

    // Ordinary ASCII should pass through unchanged
    std::string s4 = R"({"Name":"Simple ASCII"})";
    CHECK_EQ(JellyfinApi::jsonStringField(s4, "Name"), "Simple ASCII");

    // \n and \t still work
    std::string s5 = R"({"X":"line1\nline2"})";
    CHECK_EQ(JellyfinApi::jsonStringField(s5, "X"), "line1\nline2");

    std::printf("[test] Unicode escape decoding OK\n");
}

// -------------------------------------------------------------------
// Test: BitmapFont::mapCodePoint
// -------------------------------------------------------------------
static void testBitmapFontMapCodePoint()
{
    std::printf("[test] BitmapFont::mapCodePoint\n");

    CHECK(BitmapFont::mapCodePoint('A') == 'A');
    CHECK(BitmapFont::mapCodePoint(' ') == ' ');
    CHECK(BitmapFont::mapCodePoint(0x00B2) == '2');
    CHECK(BitmapFont::mapCodePoint(0x00B3) == '3');
    CHECK(BitmapFont::mapCodePoint(0x201C) == '"');
    CHECK(BitmapFont::mapCodePoint(0x201D) == '"');
    CHECK(BitmapFont::mapCodePoint(0x2014) == '-');
    CHECK(BitmapFont::mapCodePoint(0x1F600) == 0);
    CHECK(BitmapFont::mapCodePoint(0x4E16) == 0);

    std::printf("[test] BitmapFont::mapCodePoint OK\n");
}

// -------------------------------------------------------------------
// Test: buildLatestUrl includes GroupItems=false
// -------------------------------------------------------------------
static void testBuildLatestUrl()
{
    std::printf("[test] buildLatestUrl includes GroupItems=false\n");

    std::string url = JellyfinApi::buildLatestUrl(
        "https://jellyfin.example.com", "user123", 16);

    // Must contain GroupItems=false
    CHECK(url.find("GroupItems=false") != std::string::npos);
    // Must contain the other expected parameters
    CHECK(url.find("Limit=16") != std::string::npos);
    CHECK(url.find("IncludeItemTypes=Movie,Series") != std::string::npos);
    CHECK(url.find("user123") != std::string::npos);
    std::printf("[test] buildLatestUrl OK\n");
}

// -------------------------------------------------------------------
// Test: library-items URL includes pagination and existing filters
// -------------------------------------------------------------------
static void testBuildLibraryItemsUrl()
{
    std::printf("[test] buildLibraryItemsUrl pagination\n");

    std::string url = JellyfinApi::buildLibraryItemsUrl(
        "https://jellyfin.example.com", "user123", "movies", "Movie", 50, 50);

    CHECK(url.find("ParentId=movies") != std::string::npos);
    CHECK(url.find("IncludeItemTypes=Movie") != std::string::npos);
    CHECK(url.find("SortBy=SortName") != std::string::npos);
    CHECK(url.find("SortOrder=Ascending") != std::string::npos);
    CHECK(url.find("Recursive=true") != std::string::npos);
    CHECK(url.find("Fields=Overview,Genres,CommunityRating,UserData,ImageTags") !=
          std::string::npos);
    CHECK(url.find("StartIndex=50") != std::string::npos);
    CHECK(url.find("Limit=50") != std::string::npos);
    std::printf("[test] buildLibraryItemsUrl OK\n");
}

// -------------------------------------------------------------------
// B5a Test: buildImageUrl — Primary image URL
// -------------------------------------------------------------------
static void testBuildImageUrlPrimary()
{
    std::printf("[test] buildImageUrl — Primary\n");
    std::string url = buildImageUrl(
        "https://jellyfin.example.com", "abc123",
        ImageType::Primary, "tag42", 300, 200);
    CHECK(url.find("/Items/abc123/Images/Primary") != std::string::npos);
    CHECK(url.find("format=jpg") != std::string::npos);
    CHECK(url.find("maxWidth=300") != std::string::npos);
    CHECK(url.find("maxHeight=200") != std::string::npos);
    CHECK(url.find("quality=80") != std::string::npos);
    CHECK(url.find("tag=tag42") != std::string::npos);
    std::printf("[test] buildImageUrl Primary OK\n");
}

// -------------------------------------------------------------------
// B5a Test: buildImageUrl — Thumb image URL
// -------------------------------------------------------------------
static void testBuildImageUrlThumb()
{
    std::printf("[test] buildImageUrl — Thumb\n");
    std::string url = buildImageUrl(
        "http://192.168.1.50:8096", "item999",
        ImageType::Thumb, "thumbTag", 150, 100);
    CHECK(url.find("/Items/item999/Images/Thumb") != std::string::npos);
    CHECK(url.find("maxWidth=150") != std::string::npos);
    CHECK(url.find("tag=thumbTag") != std::string::npos);
    std::printf("[test] buildImageUrl Thumb OK\n");
}

// -------------------------------------------------------------------
// B5a Test: imageTypeToString and cache key differences
// -------------------------------------------------------------------
static void testImageTypeAndCacheKeys()
{
    std::printf("[test] imageTypeToString + cache keys differ\n");
    CHECK_EQ(std::string(imageTypeToString(ImageType::Primary)), "Primary");
    CHECK_EQ(std::string(imageTypeToString(ImageType::Thumb)), "Thumb");
    std::string p = ImageCache::cacheFilename("i", ImageType::Primary, "t", 100, 200);
    std::string t = ImageCache::cacheFilename("i", ImageType::Thumb, "t", 100, 200);
    CHECK(p != t);
    CHECK(p.find("_Primary_") != std::string::npos);
    CHECK(t.find("_Thumb_") != std::string::npos);
    std::printf("[test] imageTypeToString + cache keys OK\n");
}

// -------------------------------------------------------------------
// B5a Test: Cache filename determinism and contents
// -------------------------------------------------------------------
static void testCacheFilename()
{
    std::printf("[test] Cache filename\n");
    std::string a = ImageCache::cacheFilename("abc", ImageType::Primary, "tag", 100, 200);
    std::string b = ImageCache::cacheFilename("abc", ImageType::Primary, "tag", 100, 200);
    CHECK_EQ(a, b);
    CHECK(a.find("100x200") != std::string::npos);
    CHECK(a.find("abc") != std::string::npos);
    CHECK(a.find("tag") != std::string::npos);
    CHECK(a.size() >= 4 && a.substr(a.size()-4) == ".jpg");
    std::printf("[test] Cache filename OK\n");
}

// -------------------------------------------------------------------
// B5a Test: Cache write/read roundtrip
// -------------------------------------------------------------------
static void testCacheWriteRead()
{
    std::printf("[test] Cache write/read roundtrip\n");
    ImageCache::setCacheDir("test_cache_images/");
    const unsigned char data[] = {0x01, 0x02, 0x03, 0xFF, 0xFE};
    CHECK(ImageCache::writeToCache("item1", ImageType::Primary, "t", 50, 50, data, sizeof(data)));
    CHECK(ImageCache::isCached("item1", ImageType::Primary, "t", 50, 50));
    auto rb = ImageCache::readCached("item1", ImageType::Primary, "t", 50, 50);
    CHECK(rb.size() == sizeof(data));
    CHECK(std::memcmp(rb.data(), data, sizeof(data)) == 0);
    std::remove(ImageCache::cachePath("item1", ImageType::Primary, "t", 50, 50).c_str());
    ::rmdir("test_cache_images");
    ImageCache::setCacheDir("cache/images/");
    std::printf("[test] Cache write/read OK\n");
}

// -------------------------------------------------------------------
// B5a Test: JPEG decode valid fixture
// -------------------------------------------------------------------
static void testJpegDecodeValid()
{
    std::printf("[test] JPEG decode valid fixture\n");
    auto img = ImageDecoder::decodeJpegFile("tests/fixtures/tiny.jpg");
    CHECK(!img.empty());
    CHECK(img.width == 4);
    CHECK(img.height == 4);
    CHECK(img.pixels.size() == 4u * 4u * 4u);
    bool allZero = true;
    for (auto b : img.pixels) { if (b != 0) { allZero = false; break; } }
    CHECK(!allZero);
    std::printf("[test] JPEG decode valid OK\n");
}

// -------------------------------------------------------------------
// B5a Test: JPEG decode fails safely on invalid data
// -------------------------------------------------------------------
static void testJpegDecodeInvalid()
{
    std::printf("[test] JPEG decode invalid data\n");
    const unsigned char garbage[] = {0x00, 0x01, 0x02, 0x03};
    CHECK(ImageDecoder::decodeJpeg(garbage, sizeof(garbage)).empty());
    CHECK(ImageDecoder::decodeJpeg(nullptr, 0).empty());
    CHECK(ImageDecoder::decodeJpeg(nullptr, 10).empty());
    CHECK(ImageDecoder::decodeJpegFile("nonexistent.jpg").empty());
    std::printf("[test] JPEG decode invalid OK\n");
}

// -------------------------------------------------------------------
// B5a Test: BinaryHttpResponse defaults and ok()
// -------------------------------------------------------------------
static void testBinaryHttpResponse()
{
    std::printf("[test] BinaryHttpResponse\n");
    BinaryHttpResponse r;
    CHECK(r.status == 0);
    CHECK(r.data.empty());
    CHECK(!r.truncated);
    CHECK(!r.ok());
    r.status = 200; r.data = {0xFF, 0xD8};
    CHECK(r.ok());
    r.status = 404;
    CHECK(!r.ok());
    r.status = 200; r.truncated = true;
    CHECK(!r.ok());
    std::printf("[test] BinaryHttpResponse OK\n");
}

// ===================================================================
// B5b tests — Selected artwork loading logic
// ===================================================================

// B5b: Missing Primary tag → no artwork request expected
static void testNoPrimaryTagNoArtwork()
{
    std::printf("[test] B5b: no Primary tag\n");
    MediaItem item;
    item.id = "test-item-1";
    CHECK(item.imageTags.find("Primary") == item.imageTags.end());
    auto it = item.imageTags.find("Primary");
    CHECK(!(it != item.imageTags.end() && !it->second.empty()));
    std::printf("[test] B5b: no Primary tag OK\n");
}

// B5b: Primary tag present produces correct identity key
static void testArtworkIdentityKey()
{
    std::printf("[test] B5b: artwork identity key\n");
    MediaItem item;
    item.id = "movie-42";
    item.imageTags["Primary"] = "abc123";
    auto it = item.imageTags.find("Primary");
    CHECK(it != item.imageTags.end());
    std::string key = item.id + ":" + it->second;
    CHECK_EQ(key, "movie-42:abc123");

    MediaItem item2;
    item2.id = "movie-99";
    item2.imageTags["Primary"] = "abc123";
    std::string key2 = item2.id + ":" + item2.imageTags["Primary"];
    CHECK(key != key2);
    std::printf("[test] B5b: artwork identity key OK\n");
}

// B5b: Same selection does not re-trigger load
static void testArtworkLoadGuard()
{
    std::printf("[test] B5b: repeat selection guard\n");
    std::string loadedId;
    bool attempted = false;

    MediaItem item;
    item.id = "item-7";
    item.imageTags["Primary"] = "tag-aaa";
    std::string key = item.id + ":" + item.imageTags["Primary"];
    bool shouldLoad = !(attempted && loadedId == key);
    CHECK(shouldLoad);

    loadedId = key;
    attempted = true;
    shouldLoad = !(attempted && loadedId == key);
    CHECK(!shouldLoad);

    item.id = "item-8";
    key = item.id + ":" + item.imageTags["Primary"];
    shouldLoad = !(attempted && loadedId == key);
    CHECK(shouldLoad);
    std::printf("[test] B5b: repeat selection guard OK\n");
}

// B5b: Cached JPEG can be decoded
static void testCachedJpegDecodeRoundtrip()
{
    std::printf("[test] B5b: cached JPEG decode roundtrip\n");
    FILE *f = std::fopen("tests/fixtures/tiny.jpg", "rb");
    CHECK(f != nullptr);
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> jpegBytes(static_cast<size_t>(fsize));
    std::fread(jpegBytes.data(), 1, jpegBytes.size(), f);
    std::fclose(f);

    ImageCache::setCacheDir("test_b5b_cache/");
    CHECK(ImageCache::writeToCache("b5b-test", ImageType::Primary,
        "roundtrip", 72, 98, jpegBytes.data(), jpegBytes.size()));
    CHECK(ImageCache::isCached("b5b-test", ImageType::Primary, "roundtrip", 72, 98));
    auto cached = ImageCache::readCached("b5b-test", ImageType::Primary, "roundtrip", 72, 98);
    CHECK(!cached.empty());
    auto decoded = ImageDecoder::decodeJpeg(cached.data(), cached.size());
    CHECK(!decoded.empty());

    std::remove("test_b5b_cache/b5b-test_Primary_roundtrip_72x98.jpg");
    ::rmdir("test_b5b_cache");
    ImageCache::setCacheDir("cache/images/");
    std::printf("[test] B5b: cached JPEG decode roundtrip OK\n");
}

// B5b: Failed/missing load leaves empty state
static void testFailedLoadLeavesEmpty()
{
    std::printf("[test] B5b: failed load leaves empty\n");
    MediaItem item;
    item.id = "item-bad";
    item.imageTags["Primary"] = "";
    auto it = item.imageTags.find("Primary");
    CHECK(!(it != item.imageTags.end() && !it->second.empty()));

    MediaItem item2;
    item2.id = "item-none";
    it = item2.imageTags.find("Primary");
    CHECK(!(it != item2.imageTags.end() && !it->second.empty()));

    const unsigned char garbage[] = {0xFF, 0x00, 0x00, 0x00};
    CHECK(ImageDecoder::decodeJpeg(garbage, sizeof(garbage)).empty());
    std::printf("[test] B5b: failed load leaves empty OK\n");
}

// B5b: Artwork URL uses correct dimensions (72×98)
static void testArtworkUrlDimensions()
{
    std::printf("[test] B5b: artwork URL dimensions\n");
    std::string url = buildImageUrl("http://server:8096", "item-1",
        ImageType::Primary, "tag-abc", 72, 98);
    CHECK(url.find("maxWidth=72") != std::string::npos);
    CHECK(url.find("maxHeight=98") != std::string::npos);
    CHECK(url.find("quality=80") != std::string::npos);
    CHECK(url.find("tag=tag-abc") != std::string::npos);
    std::printf("[test] B5b: artwork URL dimensions OK\n");
}
// B5c1: Movie artwork box is 64×96
static void testMovieArtworkBox()
{
    std::printf("[test] B5c1: movie artwork box = 64x96\n");
    MediaItem m;
    m.type = "movie";
    auto box = artworkBoxSize(m);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: movie artwork box OK\n");
}

// B5c1: Show artwork box is 64×96
static void testShowArtworkBox()
{
    std::printf("[test] B5c1: show artwork box = 64x96\n");
    MediaItem s;
    s.type = "show";
    auto box = artworkBoxSize(s);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: show artwork box OK\n");
}

// B5c1: Episode artwork box is 128×72
static void testEpisodeArtworkBox()
{
    std::printf("[test] B5c1: episode artwork box = 128x72\n");
    MediaItem e;
    e.type = "episode";
    auto box = artworkBoxSize(e);
    CHECK(box.w == 128);
    CHECK(box.h == 72);
    std::printf("[test] B5c1: episode artwork box OK\n");
}

// B5c1: Anything-else artwork box is 64×96
static void testOtherArtworkBox()
{
    std::printf("[test] B5c1: other type artwork box = 64x96\n");
    MediaItem x;
    x.type = "folder";
    auto box = artworkBoxSize(x);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5c1: other type artwork box OK\n");
}

// B5c1: Metadata X starts after artwork box + gap
static void testMetadataXFollowsBoxWidth()
{
    std::printf("[test] B5c1: metadata X follows box width\n");
    // ART_X = 8, gap = 10, so mx = 8 + boxW + 10
    MediaItem movie;
    movie.type = "movie";
    auto mbox = artworkBoxSize(movie);
    int mxMovie = 8 + mbox.w + 10;  // 82
    CHECK(mxMovie == 82);

    MediaItem episode;
    episode.type = "episode";
    auto ebox = artworkBoxSize(episode);
    int mxEpisode = 8 + ebox.w + 10;  // 146
    CHECK(mxEpisode == 146);

    // Episode metadata starts further right than movie metadata
    CHECK(mxEpisode > mxMovie);
    std::printf("[test] B5c1: metadata X follows box width OK\n");
}

// -------------------------------------------------------------------
// B5d1: Row card geometry and scrolling tests
// -------------------------------------------------------------------

// B5d1: Movie row card is 64×96
static void testMovieRowCard()
{
    std::printf("[test] B5d1: movie row card = 64x96\n");
    MediaItem m;
    m.type = "movie";
    auto box = artworkBoxSize(m);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    CHECK(rowStripHeight() == 96);
    std::printf("[test] B5d1: movie row card OK\n");
}

// B5d1: Show row card is 64×96
static void testShowRowCard()
{
    std::printf("[test] B5d1: show row card = 64x96\n");
    MediaItem s;
    s.type = "show";
    auto box = artworkBoxSize(s);
    CHECK(box.w == 64);
    CHECK(box.h == 96);
    std::printf("[test] B5d1: show row card OK\n");
}

// B5d1: Episode row card is 128×72
static void testEpisodeRowCard()
{
    std::printf("[test] B5d1: episode row card = 128x72\n");
    MediaItem e;
    e.type = "episode";
    auto box = artworkBoxSize(e);
    CHECK(box.w == 128);
    CHECK(box.h == 72);
    std::printf("[test] B5d1: episode row card OK\n");
}

// B5d1: Mixed-width card X positions
static void testMixedWidthPositions()
{
    std::printf("[test] B5d1: mixed-width X positions\n");
    MediaItem movie;  movie.type = "movie";   // w=64
    MediaItem episode; episode.type = "episode"; // w=128
    std::vector<MediaItem> row = {movie, episode, movie};

    // startX=4, gap=6
    CHECK(cardXPosition(row, 0) == 4);                // movie
    CHECK(cardXPosition(row, 1) == 4 + 64 + 6);      // 74, episode
    CHECK(cardXPosition(row, 2) == 4 + 64 + 6 + 128 + 6); // 208, movie

    // Total width: right edge of last card
    int totalW = cardXPosition(row, 3); // past-the-end X = startX + all widths + gaps
    CHECK(totalW == 4 + 64 + 6 + 128 + 6 + 64 + 6); // 278
    // totalRowWidth excludes trailing gap
    CHECK(totalRowWidth(row) == totalW - 6); // 272
    std::printf("[test] B5d1: mixed-width X positions OK\n");
}

// B5d1: Scrolling right keeps selected card visible
static void testScrollRightKeepsVisible()
{
    std::printf("[test] B5d1: scroll right keeps selected visible\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(15, m); // 15 movies, each 64 wide

    // Card 14 starts at 4 + 14*(64+6) = 4 + 980 = 984
    // Right edge = 984 + 64 = 1048.  Viewport = 640.
    // Needed scroll = 1048 - 640 = 408
    int scroll = clampCardScroll(row, 14, 0, 640);
    CHECK(scroll == 408);
    // Verify the card is now visible: screenX = 984 - 408 = 576, right = 640
    CHECK(984 - scroll >= 0);
    CHECK(984 + 64 - scroll <= 640);
    std::printf("[test] B5d1: scroll right keeps selected visible OK\n");
}

// B5d1: Scrolling back left decreases pixel scroll
static void testScrollLeftDecreases()
{
    std::printf("[test] B5d1: scroll back left decreases scroll\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(15, m);

    // Start with card 14 visible (scroll = 408)
    int scroll = clampCardScroll(row, 14, 0, 640);
    CHECK(scroll == 408);

    // Move to card 0: cardX(0) = 4. Need scroll = 4 so screenX = 0.
    scroll = clampCardScroll(row, 0, scroll, 640);
    CHECK(scroll == 4);
    CHECK(scroll < 408); // scroll decreased
    std::printf("[test] B5d1: scroll back left decreases OK\n");
}

// B5d1: Scroll never becomes negative
static void testScrollNeverNegative()
{
    std::printf("[test] B5d1: scroll never negative\n");
    MediaItem m; m.type = "movie";
    std::vector<MediaItem> row(5, m);

    // activeCard = 0, currentScroll = -50
    int scroll = clampCardScroll(row, 0, -50, 640);
    CHECK(scroll >= 0);

    // activeCard = 2, currentScroll = 0 (small row, fits on screen)
    scroll = clampCardScroll(row, 2, 0, 640);
    CHECK(scroll >= 0);
    CHECK(scroll == 0); // card 2 at 4+2*70=144, right=208, fits

    std::printf("[test] B5d1: scroll never negative OK\n");
}

// B5d1: No card-index / pixel-offset confusion
static void testScrollIsPixelNotIndex()
{
    std::printf("[test] B5d1: scroll is pixel offset, not card index\n");
    MediaItem m; m.type = "movie"; // w=64
    std::vector<MediaItem> row(20, m);

    // Card 19 at 4 + 19*70 = 1334, right edge = 1398
    // Needed scroll = 1398 - 640 = 758
    int scroll = clampCardScroll(row, 19, 0, 640);
    CHECK(scroll == 758);
    // 758 is a pixel value, far larger than any card index (0-19)
    CHECK(scroll > 19);

    // Verify: card 19 screenX = 1334 - 758 = 576, right = 640. Visible.
    CHECK(1334 - scroll >= 0);
    CHECK(1334 + 64 - scroll <= 640);

    std::printf("[test] B5d1: scroll is pixel offset OK\n");
}

// ===================================================================
// B5d2a tests — Row artwork loading state (no rendering)
// ===================================================================

#include <map>

// Helper: simulate candidate selection (same logic as tryLoadOneRowArtwork)
static std::string pickCandidate(
    const std::vector<MediaItem> &items,
    const std::map<std::string, RowArtworkStatus> &statusMap)
{
    for (const auto &item : items) {
        std::string key = buildRowArtworkKey(item);
        if (key.empty()) continue;
        if (statusMap.find(key) == statusMap.end())
            return key;
    }
    return {};
}

// B5d2a: Movie row artwork uses Primary at 64x96
static void testMovieRowKeyPrimary()
{
    std::printf("[test] B5d2a: movie row key -> Primary 64x96\n");
    MediaItem m;
    m.id = "m1"; m.type = "movie";
    m.imageTags["Primary"] = "tagA";
    std::string key = buildRowArtworkKey(m);
    CHECK(!key.empty());
    CHECK(key.find("Primary") != std::string::npos);
    CHECK(key.find("Thumb") == std::string::npos);
    CHECK(key.find("64x96") != std::string::npos);
    CHECK(key == "m1:Primary:tagA:64x96");
    std::printf("[test] B5d2a: movie row key -> Primary 64x96 OK\n");
}

// B5d2a: Episode row artwork uses Primary at 128x72
static void testEpisodeRowKeyPrimary()
{
    std::printf("[test] B5d2a: episode row key -> Primary 128x72\n");
    MediaItem e;
    e.id = "e1"; e.type = "episode";
    e.imageTags["Primary"] = "tagB";
    std::string key = buildRowArtworkKey(e);
    CHECK(!key.empty());
    CHECK(key.find("Primary") != std::string::npos);
    CHECK(key.find("128x72") != std::string::npos);
    CHECK(key == "e1:Primary:tagB:128x72");
    std::printf("[test] B5d2a: episode row key -> Primary 128x72 OK\n");
}

// B5d2a: No Primary tag → empty key (skip that item)
static void testNoPrimaryTagEmptyKey()
{
    std::printf("[test] B5d2a: no Primary tag -> empty key\n");
    MediaItem m;
    m.id = "x1"; m.type = "movie";
    // no imageTags at all
    CHECK(buildRowArtworkKey(m).empty());
    std::printf("[test] B5d2a: no Primary tag -> empty key OK\n");
}

// B5d2a: Same key does not load twice (already in map → not a candidate)
static void testSameKeyNotLoadedTwice()
{
    std::printf("[test] B5d2a: same key not loaded twice\n");
    MediaItem m;
    m.id = "m2"; m.type = "movie";
    m.imageTags["Primary"] = "t1";
    std::string key = buildRowArtworkKey(m);

    std::map<std::string, RowArtworkStatus> sm;
    sm[key] = RowArtworkStatus::Loaded;

    std::vector<MediaItem> items = {m};
    CHECK(pickCandidate(items, sm).empty());

    sm[key] = RowArtworkStatus::Failed;
    CHECK(pickCandidate(items, sm).empty());

    std::printf("[test] B5d2a: same key not loaded twice OK\n");
}

// B5d2a: At most one candidate selected per cycle
static void testOneCandidatePerCycle()
{
    std::printf("[test] B5d2a: one candidate per cycle\n");
    std::vector<MediaItem> items;
    for (int i = 0; i < 5; ++i) {
        MediaItem m;
        char id[32]; std::snprintf(id, sizeof(id), "item-%d", i);
        m.id = id; m.type = "movie";
        char tag[32]; std::snprintf(tag, sizeof(tag), "t%d", i);
        m.imageTags["Primary"] = tag;
        items.push_back(m);
    }
    std::map<std::string, RowArtworkStatus> sm;

    std::string c1 = pickCandidate(items, sm);
    CHECK(!c1.empty());
    sm[c1] = RowArtworkStatus::Failed;
    std::string c2 = pickCandidate(items, sm);
    CHECK(!c2.empty());
    CHECK(c1 != c2);
    sm[c2] = RowArtworkStatus::Loaded;
    std::string c3 = pickCandidate(items, sm);
    CHECK(!c3.empty());
    CHECK(c3 != c1 && c3 != c2);

    std::printf("[test] B5d2a: one candidate per cycle OK\n");
}

// B5e1a: Season item with IndexNumber
static void testSeasonIndexNumber()
{
    std::printf("[test] B5e1a: Season IndexNumber parsing\n");
    std::string j = R"({"Id":"sn1","Name":"Season 1","Type":"Season","IndexNumber":1})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.id, "sn1");
    CHECK_EQ(item.title, "Season 1");
    CHECK(item.indexNumber == 1);
    std::printf("[test] B5e1a: Season IndexNumber parsing OK\n");
}

// B5e1a: Type "Season" normalised to "season"
static void testSeasonTypeNormalization()
{
    std::printf("[test] B5e1a: Season type normalization\n");
    std::string j = R"({"Id":"sn2","Name":"Season 2","Type":"Season","IndexNumber":2})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "season");
    CHECK(item.indexNumber == 2);
    std::printf("[test] B5e1a: Season type normalization OK\n");
}

// B5e2a: Episode JSON parsing — all episode metadata fields
static void testEpisodeJsonParsing()
{
    std::printf("[test] B5e2a: Episode JSON parsing\n");
    std::string j = R"({"Id":"ep1","Name":"Pilot","Type":"Episode","
        R"("IndexNumber":3,"ParentIndexNumber":2,"RunTimeTicks":6600000000,""
        R"("SeriesName":"Example Show","SeriesId":"series123","SeasonId":"season456"})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.id, "ep1");
    CHECK_EQ(item.title, "Pilot");
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 3);
    CHECK(item.parentIndexNumber == 2);
    CHECK(item.runTimeTicks == 6600000000LL);
    CHECK_EQ(item.seriesName, "Example Show");
    CHECK_EQ(item.seriesId, "series123");
    CHECK_EQ(item.seasonId, "season456");
    std::printf("[test] B5e2a: Episode JSON parsing OK\n");
}

// B5e2a: Episode type "Episode" normalised to "episode"
static void testEpisodeTypeNormalization()
{
    std::printf("[test] B5e2a: Episode type normalization\n");
    std::string j = R"({"Id":"ep2","Name":"S2E1","Type":"Episode","IndexNumber":1,"ParentIndexNumber":2})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 1);
    CHECK(item.parentIndexNumber == 2);
    std::printf("[test] B5e2a: Episode type normalization OK\n");
}

// B5e2a: ticksToMinutes helper
static void testTicksToMinutes()
{
    std::printf("[test] B5e2a: ticksToMinutes\n");
    CHECK(ticksToMinutes(0) == 0);
    CHECK(ticksToMinutes(-100) == 0);

    // 6600000000 ticks = 660 seconds = 11 minutes exactly
    CHECK(ticksToMinutes(6600000000LL) == 11);

    // 3300000000 ticks = 330 seconds = 5.5 min -> rounds to 6
    CHECK(ticksToMinutes(3300000000LL) == 6);

    // 2700000000 ticks = 270 seconds = 4.5 min -> rounds to 5
    CHECK(ticksToMinutes(2700000000LL) == 5);

    // 600000000 ticks = 60 seconds = 1 min
    CHECK(ticksToMinutes(600000000LL) == 1);

    // 300000000 ticks = 30 seconds = 0.5 min -> rounds to 1
    CHECK(ticksToMinutes(300000000LL) == 1);

    std::printf("[test] B5e2a: ticksToMinutes OK\n");
}

// B5e2a: Episode defaults (fields absent in JSON)
static void testEpisodeDefaults()
{
    std::printf("[test] B5e2a: Episode field defaults\n");
    std::string j = R"({"Id":"ep3","Name":"Intro","Type":"Episode"})";
    auto item = JellyfinApi::jsonToMediaItem(j);
    CHECK_EQ(item.type, "episode");
    CHECK(item.indexNumber == 0);
    CHECK(item.parentIndexNumber == 0);
    CHECK(item.runTimeTicks == 0LL);
    CHECK(item.seriesName.empty());
    CHECK(item.seriesId.empty());
    CHECK(item.seasonId.empty());
    std::printf("[test] B5e2a: Episode field defaults OK\n");
}

// B5e3b: findEpisodeIndex — target found at correct index
static void testFindEpisodeIndexFound()
{
    std::printf("[test] B5e3b: findEpisodeIndex found\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; a.indexNumber = 1; eps.push_back(a);
    MediaItem b; b.id = "B"; b.indexNumber = 2; eps.push_back(b);
    MediaItem c; c.id = "C"; c.indexNumber = 3; eps.push_back(c);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "B") == 1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "A") == 0);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "C") == 2);
    std::printf("[test] B5e3b: findEpisodeIndex found OK\n");
}

// B5e3b: findEpisodeIndex — unknown target returns -1
static void testFindEpisodeIndexNotFound()
{
    std::printf("[test] B5e3b: findEpisodeIndex not found\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; eps.push_back(a);
    MediaItem b; b.id = "B"; eps.push_back(b);
    MediaItem c; c.id = "C"; eps.push_back(c);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "Z") == -1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "D") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex not found OK\n");
}

// B5e3b: findEpisodeIndex — empty target returns -1
static void testFindEpisodeIndexEmpty()
{
    std::printf("[test] B5e3b: findEpisodeIndex empty target\n");
    std::vector<MediaItem> eps;
    MediaItem a; a.id = "A"; eps.push_back(a);
    MediaItem b; b.id = "B"; eps.push_back(b);

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex empty target OK\n");
}

// B5e3b: findEpisodeIndex — empty list returns -1
static void testFindEpisodeIndexEmptyList()
{
    std::printf("[test] B5e3b: findEpisodeIndex empty list\n");
    std::vector<MediaItem> eps;

    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "A") == -1);
    CHECK(EpisodeBrowserScreen::findEpisodeIndex(eps, "") == -1);
    std::printf("[test] B5e3b: findEpisodeIndex empty list OK\n");
}

// B5g1b: bounded predictive episode-artwork scheduling
static void testEpisodePrefetchScheduler()
{
    std::printf("[test] B5g1b: bounded prefetch scheduler\n");
    std::set<int> unavailable;

    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 5);
    unavailable.insert(5);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 6);
    unavailable.insert(6);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 7);

    unavailable.clear();
    for (int i = 5; i <= 13; ++i) unavailable.insert(i);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 4);

    unavailable.clear();
    unavailable.insert(0);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(0, 20, unavailable) == 1);

    unavailable.clear();
    unavailable.insert(19);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(19, 20, unavailable) == 18);

    unavailable.clear();
    unavailable.insert(5); // failed candidate
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 6);
    unavailable.insert(6); // in-progress candidate
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(5, 20, unavailable) == 7);

    // Even for 1000 episodes, nothing outside selected,+8,-3 is considered.
    unavailable.clear();
    unavailable.insert(500);
    for (int i = 501; i <= 508; ++i) unavailable.insert(i);
    unavailable.insert(499);
    unavailable.insert(498);
    unavailable.insert(497);
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(500, 1000, unavailable) == -1);

    // A drastic selection change recomputes directly; there is no backlog.
    unavailable.clear();
    CHECK(EpisodeBrowserScreen::nextPrefetchIndex(900, 1000, unavailable) == 900);
    std::printf("[test] B5g1b: bounded prefetch scheduler OK\n");
}

static void testEpisodePrefetchPlaybackResume()
{
    std::printf("[test] B5g1b: playback resume countdown\n");
    bool pending = true;
    int delayUpdates = 1;

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(pending);
    CHECK(delayUpdates == 0);

    CHECK(EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    CHECK(!pending);
    CHECK(delayUpdates == 0);

    CHECK(!EpisodeBrowserScreen::advancePrefetchResume(
        pending, delayUpdates));
    std::printf("[test] B5g1b: playback resume countdown OK\n");
}

// B5f2: PlaybackRequest — valid movie writes expected fields
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
static void testOfflineCatalog(){std::string root="/tmp/miyoofin-offline-catalog-test-"+std::to_string((long long)getpid()),p=root+"/catalog";MediaItem s=cacheItem("series");s.title="Séries 世界";MediaItem season=cacheItem("season");season.seriesId=s.id;MediaItem ep=cacheItem("episode");ep.seriesId=s.id;ep.seasonId=season.id;ep.indexNumber=2;ep.parentIndexNumber=1;
    // Season planning stores its series, season and episodes without requiring a screen visit.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));OfflineCatalogSnapshot x;CHECK(OfflineCatalog::load(p,x));CHECK(x.series[s.id].title==s.title&&x.seasonsBySeries[s.id][0].id==season.id);auto &got=x.episodesBySeason[season.id][0];CHECK(got.id==ep.id&&got.indexNumber==2&&got.parentIndexNumber==1&&got.runTimeTicks==ep.runTimeTicks&&got.playbackPositionTicks==ep.playbackPositionTicks);
    // Series planning merges all fetched seasons/episodes, retains other cached series, and de-duplicates IDs.
    MediaItem other=cacheItem("other-series"),season2=cacheItem("season-2"),ep2=cacheItem("episode-2");season2.seriesId=s.id;ep2.seriesId=s.id;ep2.seasonId=season2.id;CHECK(OfflineCatalog::storeSeasons(p,other,{cacheItem("other-season")}));CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{season,season2,season2},{{season.id,{ep,ep}},{season2.id,{ep2,ep2}}}));CHECK(OfflineCatalog::load(p,x));CHECK(x.series.count(other.id)==1&&x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1&&x.episodesBySeason[season2.id].size()==(size_t)1);
    // An incomplete planner result is a no-op and cannot erase valid cached hierarchy.
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(p,s,{},{{season.id,{}}},false));CHECK(OfflineCatalog::load(p,x));CHECK(x.seasonsBySeries[s.id].size()==(size_t)2&&x.episodesBySeason[season.id].size()==(size_t)1);
    // Corrupting metadata never rewrites it or touches a real downloaded-item fixture.
    DownloadStore store(root+"/downloads");std::string scope="fixture",itemId="downloaded-item";DownloadItem downloaded;downloaded.itemId=itemId;downloaded.expectedSize=5;downloaded.chunkSize=5;downloaded.state=DownloadState::Complete;downloaded.downloadedBytes=5;CHECK(store.saveManifest(scope,downloaded));CHECK(store.saveIndex(scope,{downloaded}));std::string chunks=store.itemPath(scope,itemId)+"/chunks";CHECK(mkdir(chunks.c_str(),0755)==0);FILE*chunk=fopen(store.chunkPath(scope,itemId,0).c_str(),"wb");fwrite("hello",1,5,chunk);fclose(chunk);std::string manifestBefore=readFixture(store.manifestPath(scope,itemId)),chunkBefore=readFixture(store.chunkPath(scope,itemId,0));FILE*f=fopen(p.c_str(),"wb");fputs("bad",f);fclose(f);std::string catalogBefore=readFixture(p);OfflineCatalogSnapshot keep=x;CHECK(!OfflineCatalog::load(p,keep));CHECK(!OfflineCatalog::storeDiscoveredHierarchy(p,s,{season},{{season.id,{ep}}}));CHECK(readFixture(p)==catalogBefore);CHECK(readFixture(store.manifestPath(scope,itemId))==manifestBefore);CHECK(readFixture(store.chunkPath(scope,itemId,0))==chunkBefore);DownloadItem loaded;CHECK(store.loadManifest(scope,itemId,loaded));CHECK(store.validateCompletedDownload(scope,loaded));std::vector<DownloadItem> index;CHECK(store.loadIndex(scope,index));CHECK(index.size()==(size_t)1&&index[0].itemId==itemId);}
static void testNewGridAndSchedule(){CHECK(moveMovieGrid(8,10,0,1)==8);CHECK(moveMovieGrid(9,10,0,-1)==9);CHECK(moveMovieGrid(8,10,1,0)==9);CHECK(clampMovieGridScroll(36,100,0)==1);MovieArtworkRange a=movieVisibleArtworkRange(0,36);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(0,37);CHECK(a.first==0&&a.lastExclusive==36);a=movieVisibleArtworkRange(1,37);CHECK(a.first==9&&a.lastExclusive==37);a=movieVisibleArtworkRange(4,100);CHECK(a.first==36&&a.lastExclusive==72);a=movieVisibleArtworkRange(9,100);CHECK(a.first==81&&a.lastExclusive==100);LibrarySyncSchedule q;CHECK(q.request(0));CHECK(!q.request(1000));CHECK(q.pending);CHECK(!q.complete(1000));CHECK(q.complete(60000));}
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

int main()
{
    std::printf("\n--- Movie title organization tests ---\n");
    testMovieOrganizationalTitles();
    testMovieAlphabetOrganization();
    testMovieOrganizationalSort();
    testMovieAlphabetFocus();
    testShowsPresentation();

    std::printf("\n--- local-first cache/grid tests ---\n");
    testLibraryCacheNew(); testOfflineCatalog(); testNewGridAndSchedule(); testCacheRemoveNew();
    std::printf("MiyooFin Checkpoint B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a tests\n");
    std::printf("==============================================================================\n\n");

    // B3 tests
    testNormaliseUrl();
    testSession();
    testSessionEmpty();
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
    testOneCandidatePerCycle();

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
    testDownloadInterruptStates();
    testDownloadPlanBatchAccounting();
    testHlsSizeEstimates(); testHlsFailureClassification();
    testDownloadsUiHelpers();
    testDownloadSourceReconciliation();

    // B5f3a tests — In-process external playback handoff
    std::printf("\n--- B5f3a external playback handoff tests ---\n");
    testExternalPlaybackFlagInitial();
    testExternalPlaybackFlagSetConsume();
    testExternalPlaybackFlagMultipleSet();
    testPlaybackRequestStillWorks();
    testScreenStackPreservedDuringExternalPlayback();

    // Central D-pad hold-to-repeat input timing
    std::printf("\n--- D-pad hold-to-repeat tests ---\n");
    testDpadHoldRepeatTiming();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B3+B4+B5a+B5b+B5c1+B5d1+B5d2a+B5e1a+B5e2a+B5e3b+B5f2+B5f3a tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
