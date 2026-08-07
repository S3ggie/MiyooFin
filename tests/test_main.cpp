// Checkpoint B3 — tests for authentication, session persistence,
// device identity, and URL normalisation.
// All tests are pure logic (no network calls).
#include <cstdio>
#include <cstring>
#include <string>
#include "miyoofin/version.hpp"
#include "../src/net/JellyfinApi.hpp"
#include "../src/net/Session.hpp"
#include "../src/net/DeviceIdentity.hpp"

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

// -------------------------------------------------------------------
// Main
// -------------------------------------------------------------------
int main()
{
    std::printf("MiyooFin Checkpoint B3 tests\n");
    std::printf("============================\n\n");

    testNormaliseUrl();
    testSession();
    testSessionEmpty();
    testDeviceIdentity();
    testDeviceIdentityLoadOrCreate();
    testAuthTypes();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B3 tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
