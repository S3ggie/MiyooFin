// -------------------------------------------------------------------
// tests/cases/test_update.inc — update module unit tests
// -------------------------------------------------------------------
#include "../../src/update/UpdateVersion.hpp"
#include "../../src/update/UpdateManifest.hpp"
#include "../../src/update/Sha256.hpp"
#include "../../src/update/UpdateInstallPlan.hpp"
#include "../../src/update/AppDir.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace miyoofin;

// ===================================================================
// UpdateVersion tests
// ===================================================================

static void testParseSemVerBasic()
{
    std::printf("[test] parseSemVer basic\n");
    SemVer v;

    CHECK(parseSemVer("0.2.0", v));
    CHECK(v.major == 0 && v.minor == 2 && v.patch == 0);
    CHECK(v.prerelease.empty());

    CHECK(parseSemVer("v0.2.0", v));
    CHECK(v.major == 0 && v.minor == 2 && v.patch == 0);

    CHECK(parseSemVer("=1.0.0", v));
    CHECK(v.major == 1 && v.minor == 0 && v.patch == 0);

    CHECK(parseSemVer("10.20.30", v));
    CHECK(v.major == 10 && v.minor == 20 && v.patch == 30);

    CHECK(parseSemVer("  3.4.5  ", v));
    CHECK(v.major == 3 && v.minor == 4 && v.patch == 5);

    // With prerelease
    CHECK(parseSemVer("0.2.0-beta.1", v));
    CHECK(v.major == 0 && v.minor == 2 && v.patch == 0);
    CHECK(v.prerelease == "beta.1");

    // With build metadata (ignored)
    CHECK(parseSemVer("1.0.0+build.123", v));
    CHECK(v.major == 1 && v.minor == 0 && v.patch == 0);
    CHECK(v.prerelease.empty());

    // With both prerelease and build metadata
    CHECK(parseSemVer("1.0.0-alpha+001", v));
    CHECK(v.prerelease == "alpha");

    std::printf("[test] parseSemVer basic OK\n");
}

static void testParseSemVerMalformed()
{
    std::printf("[test] parseSemVer malformed\n");
    SemVer v;

    CHECK(!parseSemVer("", v));
    CHECK(!parseSemVer("v", v));
    CHECK(!parseSemVer("abc", v));
    CHECK(!parseSemVer("1.2", v));
    CHECK(!parseSemVer("1.2.3.4", v));
    CHECK(!parseSemVer("1.2.x", v));
    CHECK(parseSemVer("v1.2.3 ", v));  // trailing space is trimmed
    CHECK(!parseSemVer("1.2.3-", v));   // empty prerelease after dash
    CHECK(!parseSemVer("1.2.3..4", v)); // double dot in prerelease

    // Leading zeros in numeric prerelease identifiers are forbidden (semver §11)
    CHECK(!parseSemVer("1.0.0-01", v));
    CHECK(!parseSemVer("1.0.0-beta.01", v));
    CHECK(!parseSemVer("1.0.0-0.01.0", v));
    // Single "0" is fine (not a leading zero)
    CHECK(parseSemVer("1.0.0-0", v));
    CHECK(parseSemVer("1.0.0-beta.0", v));

    // Mixed alphanumeric identifiers with leading zero are allowed
    CHECK(parseSemVer("1.0.0-0beta", v));
    CHECK(v.prerelease == "0beta");
    CHECK(parseSemVer("1.0.0-0rc1", v));
    CHECK(v.prerelease == "0rc1");
    CHECK(parseSemVer("1.0.0-0alpha.1", v));
    CHECK(v.prerelease == "0alpha.1");

    std::printf("[test] parseSemVer malformed OK\n");
}

static void testCompareSemVer()
{
    std::printf("[test] compareSemVer\n");

    CHECK(compareSemVer({0,1,0,""}, {0,2,0,""}) < 0);
    CHECK(compareSemVer({0,2,0,""}, {0,1,0,""}) > 0);
    CHECK(compareSemVer({0,2,0,""}, {0,2,0,""}) == 0);

    // Prerelease vs release
    CHECK(compareSemVer({0,2,0,""}, {0,2,0,"beta.1"}) > 0);
    CHECK(compareSemVer({0,2,0,"beta.1"}, {0,2,0,""}) < 0);

    // Prerelease ordering
    CHECK(compareSemVer({0,2,0,"beta.2"}, {0,2,0,"beta.1"}) > 0);
    CHECK(compareSemVer({0,2,0,"beta.1"}, {0,2,0,"beta.2"}) < 0);

    // Numeric prerelease identifiers compared numerically
    CHECK(compareSemVer({0,2,0,"beta.10"}, {0,2,0,"beta.9"}) > 0);
    CHECK(compareSemVer({0,2,0,"beta.9"}, {0,2,0,"beta.10"}) < 0);

    // B3/M1: More fields = higher precedence when preceding are equal
    CHECK(compareSemVer({0,2,0,"alpha"}, {0,2,0,"alpha.1"}) < 0);
    CHECK(compareSemVer({0,2,0,"alpha.1"}, {0,2,0,"alpha"}) > 0);
    CHECK(compareSemVer({0,2,0,"1"}, {0,2,0,"1.0"}) < 0);
    CHECK(compareSemVer({0,2,0,"1.0"}, {0,2,0,"1"}) > 0);

    // B3: Equal-length identical prerelease must terminate (not hang)
    CHECK(compareSemVer({0,2,0,"1.0"}, {0,2,0,"1.0"}) == 0);
    CHECK(compareSemVer({0,2,0,"alpha.beta"}, {0,2,0,"alpha.beta"}) == 0);
    CHECK(compareSemVer({0,2,0,"1"}, {0,2,0,"1"}) == 0);

    // Overflow: numeric identifiers near ULLONG_MAX must not wrap
    CHECK(compareSemVer({0,2,0,"9999999999999999999"},
                        {0,2,0,"9999999999999999998"}) > 0);
    CHECK(compareSemVer({0,2,0,"9999999999999999999"},
                        {0,2,0,"9999999999999999999"}) == 0);
    // Huge numbers saturate; ordering must remain total
    CHECK(compareSemVer({0,2,0,"999999999999999999999999999999"},
                        {0,2,0,"1"}) > 0);

    // Mixed alphanumeric vs pure numeric: alpha > numeric
    CHECK(compareSemVer({0,2,0,"0beta"}, {0,2,0,"0"}) > 0);
    CHECK(compareSemVer({0,2,0,"0"}, {0,2,0,"0beta"}) < 0);

    std::printf("[test] compareSemVer OK\n");
}

static void testIsNewerThan()
{
    std::printf("[test] isNewerThan\n");

    CHECK(isNewerThan("v0.2.0", "0.1.0"));
    CHECK(!isNewerThan("v0.1.0", "0.2.0"));
    CHECK(!isNewerThan("v0.2.0", "0.2.0"));
    CHECK(isNewerThan("0.2.0", "0.2.0-beta.1"));
    CHECK(isNewerThan("0.2.0-beta.2", "0.2.0-beta.1"));
    CHECK(isNewerThan("0.2.0-beta.10", "0.2.0-beta.9"));

    // Malformed -> false
    CHECK(!isNewerThan("abc", "0.1.0"));
    CHECK(!isNewerThan("0.1.0", "xyz"));
    CHECK(!isNewerThan("", ""));

    std::printf("[test] isNewerThan OK\n");
}

// ===================================================================
// UpdateManifest tests
// ===================================================================

static void testParseManifestValid()
{
    std::printf("[test] parseUpdateManifest valid\n");
    const char *json = R"({
        "name": "MiyooFin",
        "version": "0.2.0",
        "tag": "v0.2.0",
        "min_version": "0.1.0",
        "published_at": "2026-01-01T00:00:00Z",
        "notes": "Test release",
        "assets": {
            "tar_gz": {
                "url": "https://github.com/S3ggie/MiyooFin/releases/download/v0.2.0/miyoofin.tar.gz",
                "sha256": "ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef0123456789",
                "size": 12345678
            },
            "zip": {
                "url": "https://example.com/miyoofin.zip",
                "sha256": "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef",
                "size": 23456789
            }
        }
    })";

    UpdateManifest m;
    CHECK(parseUpdateManifest(json, m));
    CHECK_EQ(m.version, std::string("0.2.0"));
    CHECK_EQ(m.tag, std::string("v0.2.0"));
    CHECK_EQ(m.minVersion, std::string("0.1.0"));
    CHECK_EQ(m.notes, std::string("Test release"));
    CHECK_EQ(m.tarGz.url, std::string("https://github.com/S3ggie/MiyooFin/releases/download/v0.2.0/miyoofin.tar.gz"));
    CHECK_EQ(m.tarGz.sha256, std::string("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"));
    CHECK(m.tarGz.size == 12345678);
    CHECK_EQ(m.zip.url, std::string("https://example.com/miyoofin.zip"));

    std::printf("[test] parseUpdateManifest valid OK\n");
}

static void testParseManifestMissingVersion()
{
    std::printf("[test] parseUpdateManifest missing version\n");
    const char *json = R"({
        "name": "MiyooFin",
        "assets": { "tar_gz": { "url": "https://github.com/S3ggie/MiyooFin/releases/download/v1.0/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "size": 100 } }
    })";
    UpdateManifest m;
    CHECK(!parseUpdateManifest(json, m));
    std::printf("[test] parseUpdateManifest missing version OK\n");
}

static void testParseManifestBadSha()
{
    std::printf("[test] parseUpdateManifest bad sha\n");
    // SHA with non-hex characters
    const char *json = R"({
        "version": "1.0.0",
        "assets": { "tar_gz": { "url": "https://github.com/S3ggie/MiyooFin/releases/download/v1.0/miyoofin.tar.gz", "sha256": "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", "size": 100 } }
    })";
    UpdateManifest m;
    CHECK(!parseUpdateManifest(json, m));

    // SHA too short
    const char *json2 = R"({
        "version": "1.0.0",
        "assets": { "tar_gz": { "url": "https://github.com/S3ggie/MiyooFin/releases/download/v1.0/miyoofin.tar.gz", "sha256": "abc123", "size": 100 } }
    })";
    CHECK(!parseUpdateManifest(json2, m));
    std::printf("[test] parseUpdateManifest bad sha OK\n");
}

static void testParseManifestMissingSize()
{
    std::printf("[test] parseUpdateManifest missing size\n");
    const char *json = R"({
        "version": "1.0.0",
        "assets": { "tar_gz": { "url": "https://github.com/S3ggie/MiyooFin/releases/download/v1.0/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789" } }
    })";
    UpdateManifest m;
    CHECK(!parseUpdateManifest(json, m));
    std::printf("[test] parseUpdateManifest missing size OK\n");
}

static void testParseManifestReorderedKeys()
{
    std::printf("[test] parseUpdateManifest reordered keys\n");
    // Same data as valid but keys in different order and extra whitespace
    const char *json = R"({
        "assets": {
            "zip": { "size": 22, "sha256": "1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef", "url": "https://z" },
            "tar_gz": { "size": 11, "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "url": "https://github.com/S3ggie/MiyooFin/releases/download/v3.0/miyoofin.tar.gz" }
        },
        "version": "3.0.0",
        "name": "MiyooFin"
    })";
    UpdateManifest m;
    CHECK(parseUpdateManifest(json, m));
    CHECK_EQ(m.version, std::string("3.0.0"));
    CHECK_EQ(m.tarGz.url, std::string("https://github.com/S3ggie/MiyooFin/releases/download/v3.0/miyoofin.tar.gz"));
    CHECK(m.tarGz.size == 11);
    std::printf("[test] parseUpdateManifest reordered keys OK\n");
}

// ===================================================================
// Sha256 tests
// ===================================================================

static void testSha256Empty()
{
    std::printf("[test] SHA-256 empty string\n");
    Sha256 h;
    unsigned char out[32];
    h.final(out);
    // Known vector: SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    CHECK_EQ(Sha256::hex(out),
             std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    std::printf("[test] SHA-256 empty string OK\n");
}

static void testSha256Abc()
{
    std::printf("[test] SHA-256 \"abc\"\n");
    Sha256 h;
    h.update("abc", 3);
    unsigned char out[32];
    h.final(out);
    // Known vector: SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    CHECK_EQ(Sha256::hex(out),
             std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    std::printf("[test] SHA-256 \"abc\" OK\n");
}

static void testSha256MultiBlock()
{
    std::printf("[test] SHA-256 multi-block string\n");
    // SHA-256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
    // = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
    const char *msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    Sha256 h;
    h.update(msg, std::strlen(msg));
    unsigned char out[32];
    h.final(out);
    CHECK_EQ(Sha256::hex(out),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    std::printf("[test] SHA-256 multi-block string OK\n");
}

static void testSha256File()
{
    std::printf("[test] SHA-256 file hashing\n");
    // Write a temp file with known content
    const char *path = "/tmp/miyoofin_test_sha256.txt";
    {
        std::ofstream f(path, std::ios::binary);
        f << "hello world";
    }
    std::string hexResult, err;
    CHECK(sha256File(path, hexResult, err));
    // SHA256("hello world") = b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9
    CHECK_EQ(hexResult,
             std::string("b94d27b9934d3e08a52e52d7da7dabfac484efe37a5380ee9088f7ace2efcde9"));

    // Cancellation test
    std::atomic<bool> cancelled{true};
    std::string hex2, err2;
    CHECK(!sha256File(path, hex2, err2, &cancelled));
    CHECK(err2 == "cancelled");

    ::unlink(path);
    std::printf("[test] SHA-256 file hashing OK\n");
}

// ===================================================================
// UpdateInstallPlan tests
// ===================================================================

static void testIsSafeTarEntry()
{
    std::printf("[test] isSafeTarEntry\n");
    std::string norm;

    // Valid entries
    CHECK(isSafeTarEntry("MiyooFin/miyoofin", norm));
    CHECK_EQ(norm, std::string("miyoofin"));

    CHECK(isSafeTarEntry("MiyooFin/lib/libfoo.so.1", norm));
    CHECK_EQ(norm, std::string("lib/libfoo.so.1"));

    CHECK(isSafeTarEntry("MiyooFin/cacert.pem", norm));

    // Reject absolute path
    CHECK(!isSafeTarEntry("/etc/passwd", norm));

    // Reject .. in path
    CHECK(!isSafeTarEntry("MiyooFin/../evil", norm));

    // Reject missing MiyooFin/ prefix
    CHECK(!isSafeTarEntry("random/file.txt", norm));

    // Reject empty after stripping prefix
    CHECK(!isSafeTarEntry("MiyooFin/", norm));

    std::printf("[test] isSafeTarEntry OK\n");
}

static void testIsWhitelistedAppPath()
{
    std::printf("[test] isWhitelistedAppPath\n");

    CHECK(isWhitelistedAppPath("miyoofin"));
    CHECK(isWhitelistedAppPath("miyoofin-https-bridge"));
    CHECK(isWhitelistedAppPath("cacert.pem"));
    CHECK(isWhitelistedAppPath("launch.sh"));
    CHECK(isWhitelistedAppPath("config.json"));
    CHECK(isWhitelistedAppPath("icon.png"));
    CHECK(isWhitelistedAppPath("assets/placeholder.png"));
    CHECK(isWhitelistedAppPath("lib/libfoo.so"));
    CHECK(isWhitelistedAppPath("lib/libfoo.so.1"));
    CHECK(isWhitelistedAppPath("lib/libSDL2.so.0"));
    CHECK(isWhitelistedAppPath("LICENSE"));
    CHECK(isWhitelistedAppPath("THIRD_PARTY_NOTICES.md"));
    CHECK(isWhitelistedAppPath("miyoofin-perf-reporter"));
    CHECK(isWhitelistedAppPath("playback_runner.sh"));

    // Blacklisted
    CHECK(!isWhitelistedAppPath("session.txt"));
    CHECK(!isWhitelistedAppPath("cache/foo"));
    CHECK(!isWhitelistedAppPath("downloads/bar"));
    CHECK(!isWhitelistedAppPath("telemetry-logs/x"));
    CHECK(!isWhitelistedAppPath("foo.log"));
    CHECK(!isWhitelistedAppPath("screen.bmp"));
    CHECK(!isWhitelistedAppPath("backup.bak"));
    CHECK(!isWhitelistedAppPath("update-foo"));

    // Unknown paths
    CHECK(!isWhitelistedAppPath("evil_binary"));
    CHECK(!isWhitelistedAppPath("random.txt"));

    std::printf("[test] isWhitelistedAppPath OK\n");
}

static void testBuildInstallPlanBinaryLast()
{
    std::printf("[test] buildInstallPlan binary last\n");
    std::vector<std::string> listing = {
        "MiyooFin/miyoofin",
        "MiyooFin/cacert.pem",
        "MiyooFin/lib/libfoo.so",
        "MiyooFin/launch.sh",
    };
    std::string err;
    auto plan = buildInstallPlan(listing, err);
    CHECK(err.empty());
    CHECK(plan.size() == 4);
    CHECK_EQ(plan.back(), std::string("miyoofin"));
    // Libraries before scripts before binary
    CHECK(plan[0] == "lib/libfoo.so" || plan[0] == "lib/libfoo.so");
    std::printf("[test] buildInstallPlan binary last OK\n");
}

static void testBuildInstallPlanMissingBinary()
{
    std::printf("[test] buildInstallPlan missing binary\n");
    std::vector<std::string> listing = {
        "MiyooFin/cacert.pem",
        "MiyooFin/lib/libfoo.so",
    };
    std::string err;
    auto plan = buildInstallPlan(listing, err);
    CHECK(plan.empty());
    CHECK(err.find("miyoofin") != std::string::npos);
    std::printf("[test] buildInstallPlan missing binary OK\n");
}

static void testBuildInstallPlanUnsafeEntry()
{
    std::printf("[test] buildInstallPlan unsafe entry\n");
    std::vector<std::string> listing = {
        "MiyooFin/miyoofin",
        "MiyooFin/../../evil",
    };
    std::string err;
    auto plan = buildInstallPlan(listing, err);
    CHECK(plan.empty());
    CHECK(!err.empty());
    std::printf("[test] buildInstallPlan unsafe entry OK\n");
}

static void testBuildInstallPlanUserStateRejected()
{
    std::printf("[test] buildInstallPlan user-state rejected\n");
    std::vector<std::string> listing = {
        "MiyooFin/miyoofin",
        "MiyooFin/session.txt",
    };
    std::string err;
    auto plan = buildInstallPlan(listing, err);
    CHECK(plan.empty());
    CHECK(!err.empty());
    std::printf("[test] buildInstallPlan user-state rejected OK\n");
}

static void testAppDir()
{
    std::printf("[test] appDir\n");
    std::string dir;
    CHECK(appDir(dir));
    CHECK(!dir.empty());
    std::printf("[test] appDir OK (%s)\n", dir.c_str());
}

// ===================================================================
// parseUpdateManifest — allowNonHttpsAssets tests
// ===================================================================

static void testParseManifestRejectFileAsset()
{
    std::printf("[test] parseUpdateManifest rejects file:// asset by default\n");
    const char *json = R"({
        "version": "1.0.0",
        "assets": { "tar_gz": { "url": "file:///tmp/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "size": 100 } }
    })";
    UpdateManifest m;
    // Production: must reject file:// assets
    CHECK(!parseUpdateManifest(json, m));
    // Dev override: must accept file:// assets
    CHECK(parseUpdateManifest(json, m, true));
    CHECK_EQ(m.tarGz.url, std::string("file:///tmp/miyoofin.tar.gz"));
    CHECK(m.tarGz.size == 100);
    std::printf("[test] parseUpdateManifest rejects file:// asset by default OK\n");
}

static void testParseManifestAcceptFileAssetDev()
{
    std::printf("[test] parseUpdateManifest accepts file:// asset with allowNonHttpsAssets\n");
    const char *json = R"({
        "version": "2.0.0",
        "assets": { "tar_gz": { "url": "file:///home/dev/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "size": 999 } }
    })";
    UpdateManifest m;
    CHECK(parseUpdateManifest(json, m, true));
    CHECK_EQ(m.version, std::string("2.0.0"));
    CHECK_EQ(m.tarGz.url, std::string("file:///home/dev/miyoofin.tar.gz"));
    CHECK(m.tarGz.size == 999);
    std::printf("[test] parseUpdateManifest accepts file:// asset with allowNonHttpsAssets OK\n");
}

static void testParseManifestAcceptAbsolutePathAssetDev()
{
    std::printf("[test] parseUpdateManifest accepts absolute path asset with allowNonHttpsAssets\n");
    const char *json = R"({
        "version": "2.0.0",
        "assets": { "tar_gz": { "url": "/tmp/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "size": 500 } }
    })";
    UpdateManifest m;
    CHECK(parseUpdateManifest(json, m, true));
    CHECK_EQ(m.tarGz.url, std::string("/tmp/miyoofin.tar.gz"));
    std::printf("[test] parseUpdateManifest accepts absolute path asset with allowNonHttpsAssets OK\n");
}

static void testParseManifestRejectsFileAssetStrict()
{
    std::printf("[test] parseUpdateManifest strict rejects absolute path asset\n");
    const char *json = R"({
        "version": "2.0.0",
        "assets": { "tar_gz": { "url": "/tmp/miyoofin.tar.gz", "sha256": "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789", "size": 500 } }
    })";
    UpdateManifest m;
    CHECK(!parseUpdateManifest(json, m, false));
    std::printf("[test] parseUpdateManifest strict rejects absolute path asset OK\n");
}
