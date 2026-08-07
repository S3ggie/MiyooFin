// Checkpoint B2 — tests for the networking layer.
// Covers URL normalisation (pure logic, no network needed).
#include <cstdio>
#include <cstring>
#include <string>
#include "../src/net/JellyfinApi.hpp"

using namespace miyoofin;

static int g_failures = 0;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures; \
        } \
    } while (0)

static void testNormaliseUrl()
{
    std::printf("[test] JellyfinApi::normaliseUrl\n");

    // Already has scheme
    CHECK(JellyfinApi::normaliseUrl("http://192.168.1.50:8096")
          == "http://192.168.1.50:8096");

    // Missing scheme -> prepend http://
    CHECK(JellyfinApi::normaliseUrl("192.168.1.50:8096")
          == "http://192.168.1.50:8096");

    // Trailing slash removed
    CHECK(JellyfinApi::normaliseUrl("http://jellyfin.local/")
          == "http://jellyfin.local");

    // https preserved
    CHECK(JellyfinApi::normaliseUrl("https://media.example.com")
          == "https://media.example.com");

    // Whitespace trimmed
    CHECK(JellyfinApi::normaliseUrl("   http://myserver.com  ")
          == "http://myserver.com");
}

int main()
{
    std::printf("MiyooFin Checkpoint B2 tests\n");
    std::printf("============================\n");

    testNormaliseUrl();

    if (g_failures == 0) {
        std::printf("All B2 tests passed.\n");
        return 0;
    }

    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
