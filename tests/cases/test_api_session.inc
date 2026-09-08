static void testNormaliseUrl()
{
    std::printf("[test] JellyfinApi::normaliseUrl\n");

    CHECK(JellyfinApi::normaliseUrl("http://192.168.1.212:8096")
          == "http://192.168.1.212:8096");
    CHECK(JellyfinApi::normaliseUrl("192.168.1.212:8096")
          == "http://192.168.1.212:8096");
    CHECK(JellyfinApi::normaliseUrl("http://jellyfin.local/")
          == "http://jellyfin.local");
    CHECK(JellyfinApi::normaliseUrl("https://example.com")
          == "https://example.com");
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
    s1.serverId    = "server-id-42";
    s1.localServerUrl = "http://192.168.1.212:8096";
    s1.publicServerUrl = "https://public.example.com";
    s1.accessToken = "abc123token";
    s1.userId      = "user-42";
    s1.userName    = "testuser";
    s1.deviceId    = "dev-001";
    s1.manualOfflineMode = true;

    CHECK(s1.saveTo(tmpPath));

    Session s2 = Session::loadFrom(tmpPath);
    CHECK(s2.valid());
    CHECK_EQ(s2.serverUrl,   "http://server:8096");
    CHECK_EQ(s2.serverId,    "server-id-42");
    CHECK_EQ(s2.localServerUrl, "http://192.168.1.212:8096");
    CHECK_EQ(s2.publicServerUrl, "https://public.example.com");
    CHECK_EQ(s2.accessToken, "abc123token");
    CHECK_EQ(s2.userId,      "user-42");
    CHECK_EQ(s2.userName,    "testuser");
    CHECK_EQ(s2.deviceId,    "dev-001");
    CHECK(s2.manualOfflineMode);

    s2.manualOfflineMode = false;
    CHECK(s2.saveTo(tmpPath));
    CHECK(!Session::loadFrom(tmpPath).manualOfflineMode);

    s2.clear();
    CHECK(!s2.valid());
    CHECK(s2.serverUrl.empty());
    CHECK(s2.serverId.empty());
    CHECK(s2.localServerUrl.empty());
    CHECK(s2.publicServerUrl.empty());
    CHECK(s2.accessToken.empty());

    std::remove(tmpPath);
    std::printf("[test] Session save/load OK\n");
}

static void testSessionBackwardCompatibility()
{
    std::printf("[test] Session legacy compatibility\n");
    const char *tmpPath = "test_legacy_session.txt";
    std::remove(tmpPath);
    {
        FILE *f = std::fopen(tmpPath, "w");
        CHECK(f != nullptr);
        if (f) {
            std::fputs("server_url=https://public.example.com\naccess_token=token\nuser_id=user\n", f);
            std::fclose(f);
        }
    }
    Session session = Session::loadFrom(tmpPath);
    CHECK(session.valid());
    CHECK_EQ(session.serverUrl, "https://public.example.com");
    CHECK(session.serverId.empty());
    CHECK(session.localServerUrl.empty());
    CHECK(session.publicServerUrl.empty());
    CHECK(!session.manualOfflineMode);
    CHECK_EQ(LibraryCache::scopeKey(session.serverUrl, session.userId),
             LibraryCache::scopeKey("https://public.example.com", "user"));
    session.serverId = "new-server-id";
    CHECK_EQ(LibraryCache::scopeKey(session.serverUrl, session.userId),
             LibraryCache::scopeKey("https://public.example.com", "user"));
    std::remove(tmpPath);
    std::printf("[test] Session legacy compatibility OK\n");
}

static void testLocalServerIdentityVerification()
{
    std::printf("[test] local server identity verification\n");
    Session session;
    session.serverUrl = "https://public.example.com";
    session.serverId = "canonical-server";
    session.userId = "user";
    session.localServerUrl = "http://192.168.1.10:8096";
    const std::string canonicalUrl = session.serverUrl;
    const std::string scope = LibraryCache::scopeKey(session.serverUrl, session.userId);

    CHECK(JellyfinApi::serverIdsMatch(session.serverId, "canonical-server"));
    CHECK(!JellyfinApi::serverIdsMatch(session.serverId, "other-server"));
    CHECK(!JellyfinApi::serverIdsMatch(session.serverId, ""));

    // A rejected or unreachable verification never replaces the existing route.
    if (JellyfinApi::serverIdsMatch(session.serverId, "other-server"))
        session.localServerUrl = "http://192.168.1.20:8096";
    CHECK_EQ(session.localServerUrl, "http://192.168.1.10:8096");
    CHECK_EQ(session.serverUrl, canonicalUrl);
    CHECK_EQ(LibraryCache::scopeKey(session.serverUrl, session.userId), scope);

    Session lanCanonical;
    lanCanonical.serverUrl = "http://192.168.1.10:8096";
    lanCanonical.serverId = "canonical-server";
    lanCanonical.userId = "user";
    const std::string lanCanonicalUrl=lanCanonical.serverUrl;
    const std::string lanScope=LibraryCache::scopeKey(lanCanonical.serverUrl,lanCanonical.userId);
    if (JellyfinApi::serverIdsMatch(lanCanonical.serverId, "canonical-server"))
        lanCanonical.publicServerUrl = "https://public.example.com";
    CHECK_EQ(lanCanonical.publicServerUrl, "https://public.example.com");
    CHECK_EQ(lanCanonical.serverUrl, lanCanonicalUrl);
    CHECK_EQ(LibraryCache::scopeKey(lanCanonical.serverUrl,lanCanonical.userId),lanScope);
    std::printf("[test] local server identity verification OK\n");
}

static void testSystemInfoParsing()
{
    std::printf("[test] SystemInfo Id parsing\n");
    ServerInfo info;
    CHECK(JellyfinApi::parseSystemInfoResponse(
        "{\"Id\":\"server-id-42\",\"ServerName\":\"Jellyfin\",\"Version\":\"10.10.0\",\"OperatingSystem\":\"Linux\"}", info));
    CHECK_EQ(info.serverId, "server-id-42");
    CHECK_EQ(info.serverName, "Jellyfin");
    std::printf("[test] SystemInfo Id parsing OK\n");
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

static void testSessionAtomicNoTmpResidue()
{
    std::printf("[test] Session atomic save — no .tmp residue\n");

    const char *tmpPath = "test_atomic_session.txt";
    std::string tmpFile = std::string(tmpPath) + ".tmp";
    std::remove(tmpPath);
    std::remove(tmpFile.c_str());

    Session s;
    s.serverUrl   = "http://server:8096";
    s.accessToken = "tok";
    s.userId      = "u1";
    CHECK(s.saveTo(tmpPath));

    // No temp file should remain after a successful save.
    {
        FILE *probe = std::fopen(tmpFile.c_str(), "r");
        CHECK(probe == nullptr);
        if (probe) std::fclose(probe);
    }

    Session loaded = Session::loadFrom(tmpPath);
    CHECK(loaded.valid());
    CHECK_EQ(loaded.serverUrl, "http://server:8096");
    CHECK_EQ(loaded.accessToken, "tok");

    std::remove(tmpPath);
    std::remove(tmpFile.c_str());
    std::printf("[test] Session atomic save — no .tmp residue OK\n");
}

static void testSessionAtomicReplacePreservesNewContent()
{
    std::printf("[test] Session atomic save — overwrite preserves new content\n");

    const char *tmpPath = "test_atomic_replace.txt";
    std::remove(tmpPath);

    // First session.
    Session s1;
    s1.serverUrl   = "http://old:8096";
    s1.serverId    = "old-id";
    s1.accessToken = "old-token";
    s1.userId      = "old-user";
    s1.userName    = "old-name";
    s1.deviceId    = "old-dev";
    s1.manualOfflineMode = false;
    CHECK(s1.saveTo(tmpPath));

    // Overwrite with a different session.
    Session s2;
    s2.serverUrl   = "http://new:8096";
    s2.serverId    = "new-id";
    s2.localServerUrl = "http://10.0.0.1:8096";
    s2.accessToken = "new-token";
    s2.userId      = "new-user";
    s2.userName    = "new-name";
    s2.deviceId    = "new-dev";
    s2.manualOfflineMode = true;
    CHECK(s2.saveTo(tmpPath));

    // Must load the second session's data, not a corrupt mix.
    Session loaded = Session::loadFrom(tmpPath);
    CHECK(loaded.valid());
    CHECK_EQ(loaded.serverUrl, "http://new:8096");
    CHECK_EQ(loaded.serverId, "new-id");
    CHECK_EQ(loaded.localServerUrl, "http://10.0.0.1:8096");
    CHECK_EQ(loaded.accessToken, "new-token");
    CHECK_EQ(loaded.userId, "new-user");
    CHECK_EQ(loaded.userName, "new-name");
    CHECK_EQ(loaded.deviceId, "new-dev");
    CHECK(loaded.manualOfflineMode);

    std::remove(tmpPath);
    std::printf("[test] Session atomic save — overwrite preserves new content OK\n");
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
    CHECK_EQ(tabs[3].name, "Downloads");
    CHECK(tabs[3].rows[0].items.empty());  // Downloads placeholder
    CHECK_EQ(tabs[4].name, "Settings");

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
    updateContinueWatchingRow(tabs, {newItem});
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[0].items[0].id, "new");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");

    tabs = {{"Home", {{"Recently Added", {recentItem}}}}};
    updateContinueWatchingRow(tabs, {newItem});
    CHECK(tabs[0].rows.size() == 2);
    CHECK_EQ(tabs[0].rows[0].label, "Continue Watching");
    CHECK_EQ(tabs[0].rows[1].label, "Recently Added");

    updateContinueWatchingRow(tabs, {});
    CHECK(tabs[0].rows.size() == 1);
    CHECK_EQ(tabs[0].rows[0].label, "Recently Added");

    tabs = {{"Home", {{"Continue Watching", {oldItem}}}}};
    updateContinueWatchingRow(tabs, {});
    CHECK(tabs[0].rows.size() == 1);
    CHECK(tabs[0].rows[0].label.empty());
    CHECK(tabs[0].rows[0].items.empty());

    updateContinueWatchingRow(tabs, {newItem});
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
// Test: BitmapFont::mapCodePoint — Latin-1 accent transliteration
// -------------------------------------------------------------------
static void testBitmapFontMapCodePointLatinAccents()
{
    std::printf("[test] BitmapFont::mapCodePoint Latin accents\n");

    // Spanish
    CHECK(BitmapFont::mapCodePoint(0x00E1) == 'a');  // á → a
    CHECK(BitmapFont::mapCodePoint(0x00E9) == 'e');  // é → e
    CHECK(BitmapFont::mapCodePoint(0x00ED) == 'i');  // í → i
    CHECK(BitmapFont::mapCodePoint(0x00F3) == 'o');  // ó → o
    CHECK(BitmapFont::mapCodePoint(0x00FA) == 'u');  // ú → u
    CHECK(BitmapFont::mapCodePoint(0x00F1) == 'n');  // ñ → n
    CHECK(BitmapFont::mapCodePoint(0x00FC) == 'u');  // ü → u
    CHECK(BitmapFont::mapCodePoint(0x00D1) == 'N');  // Ñ → N
    CHECK(BitmapFont::mapCodePoint(0x00DC) == 'U');  // Ü → U
    CHECK(BitmapFont::mapCodePoint(0x00BF) == '?');  // ¿ → ?
    CHECK(BitmapFont::mapCodePoint(0x00A1) == '!');  // ¡ → !

    // French/German/Portuguese/Italian
    CHECK(BitmapFont::mapCodePoint(0x00E7) == 'c');  // ç → c
    CHECK(BitmapFont::mapCodePoint(0x00C7) == 'C');  // Ç → C
    CHECK(BitmapFont::mapCodePoint(0x00E4) == 'a');  // ä → a
    CHECK(BitmapFont::mapCodePoint(0x00F6) == 'o');  // ö → o
    CHECK(BitmapFont::mapCodePoint(0x00E8) == 'e');  // è → e
    CHECK(BitmapFont::mapCodePoint(0x00EA) == 'e');  // ê → e
    CHECK(BitmapFont::mapCodePoint(0x00C0) == 'A');  // À → A
    CHECK(BitmapFont::mapCodePoint(0x00C1) == 'A');  // Á → A
    CHECK(BitmapFont::mapCodePoint(0x00C9) == 'E');  // É → E
    CHECK(BitmapFont::mapCodePoint(0x00D3) == 'O');  // Ó → O
    CHECK(BitmapFont::mapCodePoint(0x00DA) == 'U');  // Ú → U
    CHECK(BitmapFont::mapCodePoint(0x00CD) == 'I');  // Í → I
    CHECK(BitmapFont::mapCodePoint(0x00D0) == 'D');  // Ð → D
    CHECK(BitmapFont::mapCodePoint(0x00F0) == 'd');  // ð → d
    CHECK(BitmapFont::mapCodePoint(0x00DE) == 'T');  // Þ → T
    CHECK(BitmapFont::mapCodePoint(0x00FE) == 't');  // þ → t

    // Existing mappings still work
    CHECK(BitmapFont::mapCodePoint('A') == 'A');
    CHECK(BitmapFont::mapCodePoint(0x00B2) == '2');
    CHECK(BitmapFont::mapCodePoint(0x201C) == '"');
    CHECK(BitmapFont::mapCodePoint(0x1F600) == 0);

    std::printf("[test] BitmapFont::mapCodePoint Latin accents OK\n");
}

// -------------------------------------------------------------------
// Test: ampersand regression + JSON escape round-trip
// -------------------------------------------------------------------
static void testAmpersandAndJsonEscape()
{
    std::printf("[test] Ampersand regression + JSON escape\n");

    // U+0026 is already ASCII '&' — bitmap font has the glyph.
    CHECK(BitmapFont::mapCodePoint(0x0026) == 0x26);
    CHECK(BitmapFont::mapCodePoint(0x0026) == '&');

    // Literal & in JSON
    std::string j1 = R"({"Name":"Tom & Jerry"})";
    CHECK_EQ(JellyfinApi::jsonStringField(j1, "Name"),
             std::string("Tom & Jerry"));

    // JSON Unicode escape \u0026 -> &
    std::string j2 = R"({"Name":"Tom \u0026 Jerry"})";
    CHECK_EQ(JellyfinApi::jsonStringField(j2, "Name"),
             std::string("Tom & Jerry"));

    // HTML entity &amp; passes through literally (no HTML decoding)
    std::string j3 = R"({"Name":"Tom &amp; Jerry"})";
    CHECK_EQ(JellyfinApi::jsonStringField(j3, "Name"),
             std::string("Tom &amp; Jerry"));

    std::printf("[test] Ampersand regression + JSON escape OK\n");
}

// -------------------------------------------------------------------
// Test: Genre JSON Unicode-escape decoding through jsonToMediaItem
// Regression: genres were extracted via jsonExtractArray + manual
// quote stripping, which did NOT run the escape decoder.
// -------------------------------------------------------------------
static void testGenreUnicodeEscapeDecoding()
{
    std::printf("[test] Genre Unicode escape decoding\n");

    // 1. Genre \\u0026 decodes to &
    {
        std::string j = R"({"Id":"s1","Name":"Key \u0026 Peele","Type":"Series","Genres":["Action \u0026 Adventure","Comedy"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK_EQ(item.genre, "Action & Adventure");
        CHECK(item.genres.size() == 2);
        CHECK_EQ(item.genres[0], "Action & Adventure");
        CHECK_EQ(item.genres[1], "Comedy");
    }

    // 2. Multiple generic Unicode escapes: \u00E9 -> é, \u00F1 -> ñ
    {
        std::string j = R"({"Id":"s2","Name":"Test","Genres":["\u00E9lan","\u00F1o\u00F1o","Action \u0026 Adventure"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK(item.genres.size() == 3);
        CHECK_EQ(item.genres[0], "\xC3\xA9lan");           // élan
        CHECK_EQ(item.genres[1], "\xC3\xB1o\xC3\xB1o");   // ñoño
        CHECK_EQ(item.genres[2], "Action & Adventure");
    }

    // 3. Title and genre in the same object both decode correctly
    {
        std::string j = R"({"Id":"s3","Name":"Fionna \u0026 Cake","Genres":["Animation \u0026 Comedy"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK_EQ(item.title, "Fionna & Cake");
        CHECK_EQ(item.genre, "Animation & Comedy");
    }

    // 4. Plain ASCII genres remain unchanged
    {
        std::string j = R"({"Id":"m1","Name":"Movie","Genres":["Drama","Thriller"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK_EQ(item.genre, "Drama");
        CHECK(item.genres.size() == 2);
        CHECK_EQ(item.genres[0], "Drama");
        CHECK_EQ(item.genres[1], "Thriller");
    }

    // 5. Empty Genres array remains empty
    {
        std::string j = R"({"Id":"m2","Name":"No Genres","Genres":[]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK(item.genre.empty());
        CHECK(item.genres.empty());
    }

    // 6. Single genre containing only \u0026 becomes "&"
    {
        std::string j = R"({"Id":"s4","Name":"Single","Genres":["\u0026"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK_EQ(item.genre, "&");
        CHECK(item.genres.size() == 1);
        CHECK_EQ(item.genres[0], "&");
    }

    // 7. Surrogate pair in genre (\uD83D\uDE00 -> U+1F600)
    {
        std::string j = R"({"Id":"s5","Name":"Emoji","Genres":["\uD83D\uDE00 Fun"]})";
        auto item = JellyfinApi::jsonToMediaItem(j);
        CHECK_EQ(item.genre, "\xF0\x9F\x98\x80 Fun");
    }

    std::printf("[test] Genre Unicode escape decoding OK\n");
}

// -------------------------------------------------------------------
// Test: BitmapFont::truncateUtf8
// -------------------------------------------------------------------
static void testBitmapFontTruncateUtf8()
{
    std::printf("[test] BitmapFont::truncateUtf8\n");

    // ASCII shorter than limit — unchanged
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 10), std::string("hello"));

    // ASCII exactly at limit — unchanged
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 5), std::string("hello"));

    // ASCII over limit — truncated with ".."
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 4), std::string("he.."));
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 3), std::string("h.."));
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 2), std::string(".."));

    // Empty string and zero limit
    CHECK_EQ(BitmapFont::truncateUtf8("", 5), std::string(""));
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 0), std::string(""));

    // UTF-8: "más" = m + á(0xC3 0xA1) + s = 3 glyphs, 4 bytes
    // á is 0xC3 0xA1 (NOT 0xC2 0xA1 which is ¡)
    std::string mas = "m\xC3\xA1s";
    CHECK_EQ(BitmapFont::truncateUtf8(mas, 10), mas);
    CHECK_EQ(BitmapFont::truncateUtf8(mas, 3), mas);

    // "abcdéfgh" = 8 glyphs. Truncate to 6: keep 4 + ".."
    std::string s1 = "abcd\xC3\xA9" "fgh";
    CHECK_EQ(BitmapFont::truncateUtf8(s1, 6), std::string("abcd.."));

    // "abácdef" = 7 glyphs. Truncate to 5: keep 3 + ".."
    std::string s2 = "ab\xC3\xA1" "cdef";
    std::string expected_s2 = "ab\xC3\xA1" "..";
    CHECK_EQ(BitmapFont::truncateUtf8(s2, 5), expected_s2);

    // "José" = 4 glyphs. Truncate to 3: keep 1 + ".."
    std::string jose = "Jos\xC3\xA9";
    CHECK_EQ(BitmapFont::truncateUtf8(jose, 3), std::string("J.."));

    // --- maxGlyphs == 1: truncated string must be "." (one column) ---
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 1), std::string("."));
    CHECK_EQ(BitmapFont::truncateUtf8("ab", 1), std::string("."));
    CHECK_EQ(BitmapFont::truncateUtf8(mas, 1), std::string("."));

    // maxGlyphs == 1 with a string that fits
    CHECK_EQ(BitmapFont::truncateUtf8("a", 1), std::string("a"));

    // maxGlyphs == 0 (negative edge: <= 0)
    CHECK_EQ(BitmapFont::truncateUtf8("hello", 0), std::string(""));
    CHECK_EQ(BitmapFont::truncateUtf8("hello", -5), std::string(""));

    // --- Truncated / incomplete multi-byte UTF-8 must not crash or overrun ---
    // Incomplete 2-byte: 0xC3 with no continuation byte
    {
        std::string bad2("\xC3", 1);           // 1 byte of a 2-byte seq = 1 glyph
        CHECK_EQ(BitmapFont::truncateUtf8(bad2, 10), bad2);  // fits: 1 glyph
        // Prepend "x" so input is 2 glyphs; truncate to 1 → "."
        std::string bad2x = std::string("x") + std::string("\xC3", 1);
        CHECK_EQ(BitmapFont::truncateUtf8(bad2x, 10), bad2x); // fits
        CHECK_EQ(BitmapFont::truncateUtf8(bad2x, 1), std::string("."));
    }

    // Incomplete 3-byte: 0xE2 with only 1 continuation byte
    {
        std::string bad3("\xE2\x80", 2);       // 2 of 3 bytes = 1 glyph
        CHECK_EQ(BitmapFont::truncateUtf8(bad3, 10), bad3);
        std::string bad3x = std::string("x") + std::string("\xE2\x80", 2);
        CHECK_EQ(BitmapFont::truncateUtf8(bad3x, 10), bad3x);
        CHECK_EQ(BitmapFont::truncateUtf8(bad3x, 1), std::string("."));
    }

    // Incomplete 4-byte: 0xF0 with only 1 continuation byte
    {
        std::string bad4("\xF0\x90", 2);       // 2 of 4 bytes = 1 glyph
        CHECK_EQ(BitmapFont::truncateUtf8(bad4, 10), bad4);
        std::string bad4x = std::string("x") + std::string("\xF0\x90", 2);
        CHECK_EQ(BitmapFont::truncateUtf8(bad4x, 10), bad4x);
        CHECK_EQ(BitmapFont::truncateUtf8(bad4x, 1), std::string("."));
    }

    // Invalid lead byte 0xFF (not a valid UTF-8 start)
    {
        std::string badff("\xFF", 1);
        CHECK_EQ(BitmapFont::truncateUtf8(badff, 10), badff);
        std::string badffx = std::string("x") + std::string("\xFF", 1);
        CHECK_EQ(BitmapFont::truncateUtf8(badffx, 10), badffx);
        CHECK_EQ(BitmapFont::truncateUtf8(badffx, 1), std::string("."));
    }

    // Bare continuation byte 0x80 (not after a lead byte)
    {
        std::string bare("\x80", 1);
        CHECK_EQ(BitmapFont::truncateUtf8(bare, 10), bare);
        std::string barex = std::string("x") + std::string("\x80", 1);
        CHECK_EQ(BitmapFont::truncateUtf8(barex, 10), barex);
        CHECK_EQ(BitmapFont::truncateUtf8(barex, 1), std::string("."));
    }

    // Valid UTF-8 followed by incomplete 2-byte — mixed input
    {
        std::string mixed = std::string("ab") + std::string("\xC3", 1);
        // "a", "b", incomplete-0xC3 = 3 glyphs; fits at 3
        CHECK_EQ(BitmapFont::truncateUtf8(mixed, 3), mixed);
        // Truncate to 2: keep 0 + ".."
        CHECK_EQ(BitmapFont::truncateUtf8(mixed, 2), std::string(".."));
        // Truncate to 1
        CHECK_EQ(BitmapFont::truncateUtf8(mixed, 1), std::string("."));
    }

    // --- Output never exceeds maxGlyphs columns ---
    // For maxGlyphs==1 the suffix is "." (1 ASCII byte) so result size == 1.
    CHECK(BitmapFont::truncateUtf8("hello", 1).size() == 1u);
    CHECK(BitmapFont::truncateUtf8(mas, 1).size() == 1u);

    std::printf("[test] BitmapFont::truncateUtf8 OK\n");
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
// Test: ChangedHierarchy uses only the affected-series projection
// -------------------------------------------------------------------
static void testChangedHierarchyLightweightProjection()
{
    std::printf("[test] ChangedHierarchy lightweight projection\n");

    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    if (listener < 0) return;

    int reuse = 1;
    CHECK(::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse)) == 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    CHECK(::bind(listener, reinterpret_cast<sockaddr *>(&address),
                 sizeof(address)) == 0);
    socklen_t addressSize = sizeof(address);
    CHECK(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                        &addressSize) == 0);
    CHECK(::listen(listener, 2) == 0);

    std::vector<std::string> requests(2);
    const auto itemJson = [](int index) {
        if (index == 0) {
            return std::string(R"({"Id":"series-0","Type":"Series",
                "Name":"ignored show name","Overview":"ignored overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-tag"}})");
        }
        if (index == 1) {
            return std::string(R"({"Id":"season-1","Type":"Season","SeriesId":"series-0",
                "Name":"ignored season name","Overview":"ignored season overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-season-tag"},
                "SeasonId":"ignored-season","RunTimeTicks":123456789})");
        }
        if (index == 2) {
            return std::string(R"({"Id":"episode-2","Type":"Episode","SeriesId":"series-0",
                "Name":"ignored episode name","Overview":"ignored episode overview",
                "UserData":{"Played":true},"ImageTags":{"Primary":"ignored-episode-tag"},
                "SeasonId":"ignored-season","RunTimeTicks":987654321})");
        }
        return "{\"Id\":\"episode-" + std::to_string(index) +
               "\",\"Type\":\"Episode\",\"SeriesId\":\"series-0\"}";
    };
    const auto pageBody = [&](int start, int count) {
        std::string body = R"({"Items":[)";
        for (int index = start; index < start + count; ++index) {
            if (index != start) body += ',';
            body += itemJson(index);
        }
        body += "]}";
        return body;
    };
    const std::string firstBody = pageBody(0, 100);
    const std::string finalBody = pageBody(100, 3);
    std::thread server([&]() {
        for (int page = 0; page < 2; ++page) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0) return;
            char buffer[4096];
            std::string captured;
            while (captured.find("\r\n\r\n") == std::string::npos) {
                const ssize_t received = ::recv(client, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                captured.append(buffer, static_cast<size_t>(received));
            }
            requests[static_cast<size_t>(page)] = captured;
            const std::string &body = page == 0 ? firstBody : finalBody;
            const std::string response =
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
            (void)::send(client, response.data(), response.size(), 0);
            ::close(client);
        }
    });

    const auto port = ntohs(address.sin_port);
    std::vector<MediaItem> items;
    std::string error;
    const bool ok = JellyfinApi::getChangedHierarchyItems(
        "http://127.0.0.1:" + std::to_string(port), "test-token", "user123",
        "device123", 1700000000123LL, items, error);
    server.join();
    ::close(listener);
    CHECK(ok);
    CHECK(error.empty());
    CHECK(items.size() == 103);
    if (items.size() == 103) {
        for (int index = 0; index < 103; ++index) {
            const std::string expectedId = index == 0 ? "series-0" :
                index == 1 ? "season-1" :
                index == 2 ? "episode-2" : "episode-" + std::to_string(index);
            CHECK_EQ(items[static_cast<size_t>(index)].id, expectedId);
        }

        // These are the exact inputs used by HomeScreen's affected-series
        // reduction: Series uses its own ID, while Season/Episode use SeriesId.
        CHECK_EQ(items[0].id, "series-0");
        CHECK_EQ(items[0].type, "show");
        const auto affectedSeriesId = [](const MediaItem &item) {
            return item.type == "show" ? item.id : item.seriesId;
        };
        CHECK_EQ(affectedSeriesId(items[0]), "series-0");
        CHECK(items[0].title.empty());
        CHECK(items[0].overview.empty());
        CHECK(items[0].imageTags.empty());
        CHECK(!items[0].played);

        CHECK_EQ(items[1].id, "season-1");
        CHECK_EQ(items[1].type, "season");
        CHECK_EQ(items[1].seriesId, "series-0");
        CHECK_EQ(affectedSeriesId(items[1]), "series-0");
        CHECK(items[1].title.empty());
        CHECK(items[1].overview.empty());
        CHECK(items[1].imageTags.empty());
        CHECK(!items[1].played);
        CHECK(items[1].seasonId.empty());
        CHECK(items[1].runTimeTicks == 0);

        CHECK_EQ(items[2].id, "episode-2");
        CHECK_EQ(items[2].type, "episode");
        CHECK_EQ(items[2].seriesId, "series-0");
        CHECK_EQ(affectedSeriesId(items[2]), "series-0");
        CHECK(items[2].title.empty());
        CHECK(items[2].overview.empty());
        CHECK(items[2].imageTags.empty());
        CHECK(!items[2].played);
        CHECK(items[2].seasonId.empty());
        CHECK(items[2].runTimeTicks == 0);
    }
    CHECK(requests[0].find("IncludeItemTypes=Series,Season,Episode") != std::string::npos);
    CHECK(requests[0].find("SortBy=DateLastSaved") != std::string::npos);
    CHECK(requests[0].find("SortOrder=Ascending") != std::string::npos);
    CHECK(requests[0].find("MinDateLastSaved=2023-11-14T22:13:20.0000000Z") !=
          std::string::npos);
    CHECK(requests[0].find("StartIndex=0") != std::string::npos);
    CHECK(requests[0].find("Limit=100") != std::string::npos);
    CHECK(requests[0].find("Fields=SeriesId") != std::string::npos);
    CHECK(requests[0].find("Fields=Overview") == std::string::npos);
    CHECK(requests[0].find("UserData") == std::string::npos);
    CHECK(requests[0].find("ImageTags") == std::string::npos);
    CHECK(requests[1].find("StartIndex=100") != std::string::npos);
    CHECK(requests[1].find("Limit=100") != std::string::npos);
    CHECK(requests[1].find("Fields=SeriesId") != std::string::npos);
    CHECK(requests[1].find("Fields=Overview") == std::string::npos);
    CHECK(requests[1].find("UserData") == std::string::npos);
    CHECK(requests[1].find("ImageTags") == std::string::npos);
    std::printf("[test] ChangedHierarchy lightweight projection OK\n");
}

// -------------------------------------------------------------------
// B5a Test: buildImageUrl — Primary image URL
// -------------------------------------------------------------------
