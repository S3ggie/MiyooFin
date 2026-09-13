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


