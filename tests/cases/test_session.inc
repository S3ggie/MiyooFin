static void testSession()
{
    std::printf("[test] Session save/load\n");

    const char *tmpPath = "test_session.txt";
    std::remove(tmpPath);

    Session s1;
    s1.serverUrl   = "http://server:8096";
    s1.serverId    = "server-id-42";
    s1.localServerUrl = "http://192.168.1.10:8096";
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
    CHECK_EQ(s2.localServerUrl, "http://192.168.1.10:8096");
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

static void testSessionRoutes()
{
    std::printf("[test] Session routes\n");
    // Older shape: the address typed at sign-in plus at most one extra.
    Session publicOnly;
    publicOnly.serverUrl = "https://public.example.com";
    CHECK(publicOnly.routes().lan.empty() && publicOnly.routes().pub == publicOnly.serverUrl);
    Session lanOnly;
    lanOnly.serverUrl = "http://192.168.1.5:8096";
    CHECK(lanOnly.routes().lan == lanOnly.serverUrl && lanOnly.routes().pub.empty());
    Session publicPlusLan = publicOnly;
    publicPlusLan.localServerUrl = "http://192.168.1.5:8096";
    CHECK(publicPlusLan.routes().lan == "http://192.168.1.5:8096" &&
          publicPlusLan.routes().pub == "https://public.example.com");
    Session lanPlusPublic = lanOnly;
    lanPlusPublic.publicServerUrl = "https://public.example.com";
    CHECK(lanPlusPublic.routes().lan == lanOnly.serverUrl &&
          lanPlusPublic.routes().pub == "https://public.example.com");

    // Converting keeps the effective addresses and the identity.
    Session converted = lanPlusPublic;
    converted.makeRoutesExplicit();
    CHECK(converted.routesExplicit && converted.serverUrl == lanOnly.serverUrl);
    CHECK(converted.routes().lan == lanPlusPublic.routes().lan &&
          converted.routes().pub == lanPlusPublic.routes().pub);

    // Explicit addresses are independent: drop either one, never touching the identity.
    const std::string identity = converted.serverUrl;
    const std::string scope = LibraryCache::scopeKey(converted.serverUrl, "user");
    converted.setRoutes("", "https://public.example.com"); // home network address removed
    CHECK(converted.routes().lan.empty() && converted.routes().pub == "https://public.example.com");
    CHECK(converted.serverUrl == identity && LibraryCache::scopeKey(converted.serverUrl, "user") == scope);
    converted.setRoutes("http://192.168.1.9:8096", ""); // and the other way round
    CHECK(converted.routes().lan == "http://192.168.1.9:8096" && converted.routes().pub.empty());
    converted.setRoutes("", ""); // never leaves a signed-in session without any address
    CHECK(converted.routes().pub == identity);

    // Saved and reloaded exactly as set, including a removed address.
    const std::string path = "test_session_routes.txt";
    Session saved = publicOnly;
    saved.accessToken = "t";
    saved.userId = "u";
    saved.setRoutes("http://192.168.1.5:8096", "");
    CHECK(saved.saveTo(path));
    Session loaded = Session::loadFrom(path);
    CHECK(loaded.routesExplicit && loaded.routes().lan == "http://192.168.1.5:8096" &&
          loaded.routes().pub.empty() && loaded.serverUrl == "https://public.example.com");
    std::remove(path.c_str());
    // A session file from before this existed loads in the older shape, then converts once.
    Session old = lanPlusPublic;
    old.accessToken = "t";
    old.userId = "u";
    CHECK(old.saveTo(path));
    Session oldLoaded = Session::loadFrom(path);
    CHECK(!oldLoaded.routesExplicit && oldLoaded.routes().pub == "https://public.example.com");
    oldLoaded.makeRoutesExplicit();
    CHECK(oldLoaded.saveTo(path) && Session::loadFrom(path).routesExplicit);
    std::remove(path.c_str());
    oldLoaded.clear();
    CHECK(!oldLoaded.routesExplicit);

    // Requests use the two addresses as set: home network first, internet as the fallback.
    Session both;
    both.serverUrl = "https://public.example.com";
    both.setRoutes("http://192.168.1.5:8096", "https://other.example.com");
    std::string error;
    std::vector<std::string> tried;
    CHECK(!RouteRequest(both).run(
        [&](const std::string& base) {
            tried.push_back(base);
            error = "Transport: Couldn't connect";
            return false;
        },
        error));
    CHECK(tried.size() == 2 && tried[0] == "http://192.168.1.5:8096" &&
          tried[1] == "https://other.example.com");
    Session homeOnly = both;
    homeOnly.setRoutes("http://192.168.1.5:8096", "");
    tried.clear();
    error.clear();
    CHECK(!RouteRequest(homeOnly).run(
        [&](const std::string& base) {
            tried.push_back(base);
            error = "Transport: Couldn't connect";
            return false;
        },
        error));
    CHECK(tried.size() == 1 && tried[0] == "http://192.168.1.5:8096"); // no phantom fallback
    Session internetOnly = both;
    internetOnly.setRoutes("", "https://other.example.com");
    tried.clear();
    CHECK(RouteRequest(internetOnly).run(
        [&](const std::string& base) {
            tried.push_back(base);
            return true;
        },
        error));
    CHECK(tried.size() == 1 && tried[0] == "https://other.example.com" &&
          RouteRequest(internetOnly).primary() == "https://other.example.com");
    std::printf("[test] Session routes OK\n");
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

    JellyfinApi::QuickConnectRequest qc;
    CHECK(JellyfinApi::parseQuickConnectInitiate(
        "{\"Authenticated\":false,\"Secret\":\"abc123\",\"Code\":\"482913\"}", qc));
    CHECK_EQ(qc.secret, "abc123");
    CHECK_EQ(qc.code, "482913");
    CHECK(!JellyfinApi::parseQuickConnectInitiate("{}", qc));
    CHECK(JellyfinApi::parseQuickConnectApproved("{\"Authenticated\": true,\"Secret\":\"x\"}"));
    CHECK(!JellyfinApi::parseQuickConnectApproved("{\"Authenticated\":false}"));
    CHECK(!JellyfinApi::parseQuickConnectApproved("{}"));
    AuthResult auth;
    CHECK(JellyfinApi::parseAuthResult(
        "{\"User\":{\"Name\":\"sam\",\"Id\":\"u1\"},\"AccessToken\":\"tok\",\"ServerId\":\"s1\"}",
        auth));
    CHECK_EQ(auth.userName, "sam");
    CHECK_EQ(auth.accessToken, "tok");
    CHECK(!JellyfinApi::parseAuthResult("{\"User\":{}}", auth));
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
// Test: DeviceIdentity fallback entropy (unreadable /dev/urandom path)
// -------------------------------------------------------------------
static void testDeviceIdentityFallbackEntropy()
{
    std::printf("[test] DeviceIdentity fallback entropy\n");

    // The fallback seed must never throw, even where std::random_device
    // cannot open /dev/urandom (non-root OnionOS ships it 0660 root:root).
    uint64_t s1 = 0, s2 = 0;
    try {
        s1 = DeviceIdentity::fallbackSeed();
        s2 = DeviceIdentity::fallbackSeed();
    } catch (...) {
        CHECK(false);
    }
    CHECK(s1 != s2);  // per-call counter keeps rapid calls distinct

    // Same seed -> same UUID; distinct seeds -> distinct UUIDs, all v4-shaped.
    std::string u1, u2, u3;
    try {
        u1 = DeviceIdentity::uuidFromSeed(s1);
        u2 = DeviceIdentity::uuidFromSeed(s1);
        u3 = DeviceIdentity::uuidFromSeed(s2);
    } catch (...) {
        CHECK(false);
    }
    CHECK_EQ(u1, u2);
    CHECK(u1 != u3);
    for (const std::string *u : {&u1, &u3}) {
        CHECK(u->size() == 36);
        CHECK((*u)[8] == '-');
        CHECK((*u)[13] == '-');
        CHECK((*u)[18] == '-');
        CHECK((*u)[23] == '-');
        CHECK((*u)[14] == '4');  // version 4
        char variant = (*u)[19];
        CHECK(variant == '8' || variant == '9' || variant == 'a' || variant == 'b');
    }

    // The full generation path must not throw either.
    std::string g;
    try {
        g = DeviceIdentity::generateUuidV4();
    } catch (...) {
        CHECK(false);
    }
    CHECK(g.size() == 36);
    CHECK(g[14] == '4');

    std::printf("[test] DeviceIdentity fallback entropy OK\n");
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
    CHECK(std::string(VERSION_STR)
          == std::to_string(VERSION_MAJOR) + "."
             + std::to_string(VERSION_MINOR) + "."
             + std::to_string(VERSION_PATCH));

    std::printf("[test] AuthResult/AuthError types OK\n");
}

// ===================================================================
// Checkpoint B4 tests
// ===================================================================


