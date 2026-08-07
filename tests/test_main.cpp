// Checkpoint B3+B4+B5a+B5b — tests for authentication, session persistence,
// device identity, URL normalisation, B4 JSON parsing/tab building,
// B5a artwork infrastructure, and B5b selected artwork loading.
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
#include "../src/image/ImageDecoder.hpp"
#include "../src/cache/ImageCache.hpp"
#include "../src/net/HttpClient.hpp"
#include "../src/ui/ArtworkLayout.hpp"
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
        "UserData":{"Played":false,"PlayedPercentage":45.5}})";
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
    CHECK(item.imageTags.size() == 1);
    CHECK_EQ(item.imageTags.at("Primary"), "tag1");

    // Series type normalization
    std::string j2 = R"({"Id":"s1","Name":"Show","Type":"Series"})";
    CHECK_EQ(JellyfinApi::jsonToMediaItem(j2).type, "show");

    // Minimal fields
    std::string j3 = R"({"Id":"m1","Name":"Min"})";
    auto m = JellyfinApi::jsonToMediaItem(j3);
    CHECK(m.year == 0); CHECK(m.genres.empty());
    CHECK(m.imageTags.empty());
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

int main()
{
    std::printf("MiyooFin Checkpoint B3+B4+B5a+B5b+B5c1 tests\n");
    std::printf("============================================\n\n");

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
    testLatestItemsDirectArray();
    testBuildLatestUrl();
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

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B3+B4+B5a+B5b+B5c1 tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
