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
#include "../src/net/HttpClient.hpp"
#include "../src/ui/ArtworkLayout.hpp"
#include "../src/ui/screens/EpisodeBrowserScreen.hpp"
#include "../src/app/ScreenStack.hpp"
#include "../src/playback/PlaybackRequest.hpp"
#include "../src/input/InputManager.hpp"
#include <unistd.h>

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

int main()
{
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
