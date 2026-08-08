// tests/test_playback_reporter.cpp — Unit tests for B5f3b playback reporting.
// Tests the pure parsing/conversion helpers from playback_clock_parser.hpp.
// No network calls. No FFplay dependency. No SDL dependency.
#include <cstdio>
#include <cstring>
#include <string>
#include <cmath>
#include <cstdint>
#include <limits>
#include "../tools/playback_clock_parser.hpp"

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

// A: seconds_to_ticks tests
static void testTicksZero() {
    std::printf("[test] seconds_to_ticks: 0.0\n");
    CHECK(seconds_to_ticks(0.0) == 0);
    std::printf("[test] seconds_to_ticks: 0.0 OK\n");
}
static void testTicks525() {
    std::printf("[test] seconds_to_ticks: 5.25\n");
    CHECK(seconds_to_ticks(5.25) == 52500000);
    std::printf("[test] seconds_to_ticks: 5.25 OK\n");
}
static void testTicks60() {
    std::printf("[test] seconds_to_ticks: 60.0\n");
    CHECK(seconds_to_ticks(60.0) == 600000000);
    std::printf("[test] seconds_to_ticks: 60.0 OK\n");
}
static void testTicks138742() {
    std::printf("[test] seconds_to_ticks: 1387.42\n");
    CHECK(seconds_to_ticks(1387.42) == 13874200000LL);
    std::printf("[test] seconds_to_ticks: 1387.42 OK\n");
}
static void testTicksNegativeClamp() {
    std::printf("[test] seconds_to_ticks: negative clamps to 0\n");
    CHECK(seconds_to_ticks(-1.0) == 0);
    CHECK(seconds_to_ticks(-100.5) == 0);
    std::printf("[test] seconds_to_ticks: negative clamps OK\n");
}
static void testTicksNaN() {
    std::printf("[test] seconds_to_ticks: NaN/Inf clamps to 0\n");
    CHECK(seconds_to_ticks(std::nan("")) == 0);
    CHECK(seconds_to_ticks(std::numeric_limits<double>::infinity()) == 0);
    std::printf("[test] seconds_to_ticks: NaN/Inf clamps OK\n");
}

// B: Valid showinfo pts_time parsing
static void testParsePts1() {
    std::printf("[test] parse_showinfo_pts: pts_time:1.46673\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] n:   0 pts:  47107 pts_time:1.46673 ...", pts));
    CHECK(std::fabs(pts - 1.46673) < 0.0001);
    std::printf("[test] parse_showinfo_pts: 1.46673 OK\n");
}
static void testParsePts2() {
    std::printf("[test] parse_showinfo_pts: pts_time:6.47173\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] n:   1 pts: 208057 pts_time:6.47173 ...", pts));
    CHECK(std::fabs(pts - 6.47173) < 0.0001);
    std::printf("[test] parse_showinfo_pts: 6.47173 OK\n");
}
static void testParsePts21() {
    std::printf("[test] parse_showinfo_pts: pts_time:21.4867\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] n:  13 pts: 693784 pts_time:21.4867 ...", pts));
    CHECK(std::fabs(pts - 21.4867) < 0.0001);
    std::printf("[test] parse_showinfo_pts: 21.4867 OK\n");
}
static void testParsePtsInteger() {
    std::printf("[test] parse_showinfo_pts: pts_time:30\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] pts_time:30 foo", pts));
    CHECK(std::fabs(pts - 30.0) < 0.001);
    std::printf("[test] parse_showinfo_pts: 30 OK\n");
}
static void testParsePtsZero() {
    std::printf("[test] parse_showinfo_pts: pts_time:0.0\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] pts_time:0.0 n:0", pts));
    CHECK(std::fabs(pts - 0.0) < 0.001);
    std::printf("[test] parse_showinfo_pts: 0.0 OK\n");
}
static void testParsePtsLarge() {
    std::printf("[test] parse_showinfo_pts: pts_time:1387.42\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("...pts_time:1387.42 ...", pts));
    CHECK(std::fabs(pts - 1387.42) < 0.001);
    std::printf("[test] parse_showinfo_pts: 1387.42 OK\n");
}

// C: Invalid records / rejection tests
static void testRejectVersionLine() {
    std::printf("[test] parse_showinfo_pts: reject version line\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("ffmpeg version 4.2.1 ...", pts));
    std::printf("[test] parse_showinfo_pts: version line OK\n");
}
static void testRejectLibavcodec() {
    std::printf("[test] parse_showinfo_pts: reject libavcodec line\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("  built with gcc ...", pts));
    std::printf("[test] parse_showinfo_pts: libavcodec OK\n");
}
static void testRejectNegative() {
    std::printf("[test] parse_showinfo_pts: reject negative pts_time\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("... pts_time:-1.5 ...", pts));
    std::printf("[test] parse_showinfo_pts: negative OK\n");
}
static void testRejectEmpty() {
    std::printf("[test] parse_showinfo_pts: reject empty string\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("", pts));
    std::printf("[test] parse_showinfo_pts: empty OK\n");
}
static void testRejectNoPtsTime() {
    std::printf("[test] parse_showinfo_pts: reject line without pts_time\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("[Parsed_showinfo_3 @ 0x...] n:   0 pts:  47107", pts));
    std::printf("[test] parse_showinfo_pts: no pts_time OK\n");
}
static void testRejectPtsTimeOnly() {
    std::printf("[test] parse_showinfo_pts: reject pts_time: without value\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("...pts_time: ...", pts));
    std::printf("[test] parse_showinfo_pts: pts_time only OK\n");
}
static void testRejectPtsTimeSign() {
    std::printf("[test] parse_showinfo_pts: reject pts_time:+5.0\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("... pts_time:+5.0 ...", pts));
    std::printf("[test] parse_showinfo_pts: explicit positive sign OK\n");
}
static void testRejectInf() {
    std::printf("[test] parse_showinfo_pts: reject pts_time:inf\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("...pts_time:inf ...", pts));
    std::printf("[test] parse_showinfo_pts: inf OK\n");
}
static void testRejectNan() {
    std::printf("[test] parse_showinfo_pts: reject pts_time:nan\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("...pts_time:nan ...", pts));
    std::printf("[test] parse_showinfo_pts: nan OK\n");
}
static void testRejectRandomLine() {
    std::printf("[test] parse_showinfo_pts: reject random unrelated line\n");
    double pts = -1.0;
    CHECK(!parse_showinfo_pts("Stream mapping:", pts));
    CHECK(!parse_showinfo_pts("Stream #0:0: Video: h264, yuv420p, 640x480", pts));
    CHECK(!parse_showinfo_pts("frame=  120 fps= 29 q=28.0 size=   1024kB time=00:00:04.00", pts));
    std::printf("[test] parse_showinfo_pts: random line OK\n");
}

// D: Seek / backward PTS behavior
static void testSeekForward() {
    std::printf("[test] parse_showinfo_pts: seek forward (PTS jumps ahead)\n");
    double pts1 = -1.0, pts2 = -1.0;
    CHECK(parse_showinfo_pts("...pts_time:5.0 ...", pts1));
    CHECK(parse_showinfo_pts("...pts_time:120.0 ...", pts2));
    CHECK(std::fabs(pts2 - 120.0) < 0.001);
    std::printf("[test] parse_showinfo_pts: seek forward OK\n");
}
static void testSeekBackward() {
    std::printf("[test] parse_showinfo_pts: seek backward (PTS smaller than previous)\n");
    double pts1 = -1.0, pts2 = -1.0;
    CHECK(parse_showinfo_pts("...pts_time:200.0 ...", pts1));
    CHECK(parse_showinfo_pts("...pts_time:30.0 ...", pts2));
    CHECK(std::fabs(pts2 - 30.0) < 0.001);
    std::printf("[test] parse_showinfo_pts: seek backward OK\n");
}
static void testNonMonotonic() {
    std::printf("[test] parse_showinfo_pts: non-monotonic sequence accepted\n");
    double pts;
    CHECK(parse_showinfo_pts("...pts_time:100.0 ...", pts));
    CHECK(parse_showinfo_pts("...pts_time:50.0 ...", pts));
    CHECK(parse_showinfo_pts("...pts_time:75.0 ...", pts));
    CHECK(std::fabs(pts - 75.0) < 0.001);
    std::printf("[test] parse_showinfo_pts: non-monotonic OK\n");
}

// E: Jellyfin ticks conversion from parsed PTS
static void testTicksFromPts21() {
    std::printf("[test] pts_time:21.4867 -> Jellyfin ticks\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("...pts_time:21.4867 ...", pts));
    int64_t ticks = seconds_to_ticks(pts);
    CHECK(ticks == 214867000);
    std::printf("[test] pts_time:21.4867 -> %lld ticks OK\n", (long long)ticks);
}
static void testTicksFromPts1() {
    std::printf("[test] pts_time:1.46673 -> Jellyfin ticks\n");
    double pts = -1.0;
    CHECK(parse_showinfo_pts("...pts_time:1.46673 ...", pts));
    int64_t ticks = seconds_to_ticks(pts);
    CHECK(ticks == 14667300);
    std::printf("[test] pts_time:1.46673 -> %lld ticks OK\n", (long long)ticks);
}

// F: record extraction (CR/LF)
static void testExtractLF() {
    std::printf("[test] extract_record: LF\n");
    std::string buf = "line1\nline2\n";
    size_t pos = 0; std::string rec;
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line1"));
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line2"));
    CHECK(!extract_record(buf, pos, rec));
    std::printf("[test] extract_record: LF OK\n");
}
static void testExtractCR() {
    std::printf("[test] extract_record: CR\n");
    std::string buf = "line1\rline2\r";
    size_t pos = 0; std::string rec;
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line1"));
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line2"));
    CHECK(!extract_record(buf, pos, rec));
    std::printf("[test] extract_record: CR OK\n");
}
static void testExtractCRLF() {
    std::printf("[test] extract_record: CRLF\n");
    std::string buf = "line1\r\nline2\r\n";
    size_t pos = 0; std::string rec;
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line1"));
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("line2"));
    CHECK(!extract_record(buf, pos, rec));
    std::printf("[test] extract_record: CRLF OK\n");
}
static void testExtractMixed() {
    std::printf("[test] extract_record: mixed CR/LF\n");
    std::string buf = "a\rb\nc\r\nd";
    size_t pos = 0; std::string rec;
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("a"));
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("b"));
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("c"));
    CHECK(!extract_record(buf, pos, rec));
    std::printf("[test] extract_record: mixed OK\n");
}
static void testExtractIncomplete() {
    std::printf("[test] extract_record: incomplete\n");
    std::string buf = "partial record";
    size_t pos = 0; std::string rec;
    CHECK(!extract_record(buf, pos, rec));
    CHECK(pos == 0);
    std::printf("[test] extract_record: incomplete OK\n");
}
static void testExtractFromOffset() {
    std::printf("[test] extract_record: from offset\n");
    std::string buf = "skip this\nkeep this\n";
    size_t pos = 10; std::string rec;
    CHECK(extract_record(buf, pos, rec)); CHECK_EQ(rec, std::string("keep this"));
    std::printf("[test] extract_record: from offset OK\n");
}

// G: pts_event lifecycle (start-once bug fix)
static void testPtsEventFirstReturnsStart() {
    std::printf("[test] pts_event: first call returns false (Start)\n");
    bool attempted = false;
    CHECK(!pts_event(attempted));  // false = caller should send Start
    CHECK(attempted);
    std::printf("[test] pts_event: first call OK\n");
}
static void testPtsEventSecondReturnsProgress() {
    std::printf("[test] pts_event: second call returns true (Progress)\n");
    bool attempted = false;
    pts_event(attempted);  // first → Start
    CHECK(pts_event(attempted));  // second → Progress
    std::printf("[test] pts_event: second call OK\n");
}
static void testPtsEventAlwaysStartThenProgress() {
    std::printf("[test] pts_event: Start once, then Progress forever\n");
    bool attempted = false;
    // Simulate 5 sampled PTS events
    CHECK(!pts_event(attempted));   // 1st → Start
    CHECK(pts_event(attempted));    // 2nd → Progress
    CHECK(pts_event(attempted));    // 3rd → Progress
    CHECK(pts_event(attempted));    // 4th → Progress
    CHECK(pts_event(attempted));    // 5th → Progress
    // Even after many calls, Start is never sent again
    for (int i = 0; i < 10; ++i)
        CHECK(pts_event(attempted));
    std::printf("[test] pts_event: always Start then Progress OK\n");
}

// ================================================================
// Main
// ================================================================
int main()
{
    std::printf("B5f3b Playback Reporter Tests\n");
    std::printf("=============================\n\n");

    std::printf("--- A: seconds_to_ticks ---\n");
    testTicksZero(); testTicks525(); testTicks60(); testTicks138742();
    testTicksNegativeClamp(); testTicksNaN();

    std::printf("\n--- B: valid showinfo pts_time parsing ---\n");
    testParsePts1(); testParsePts2(); testParsePts21();
    testParsePtsInteger(); testParsePtsZero(); testParsePtsLarge();

    std::printf("\n--- C: invalid records / rejection ---\n");
    testRejectVersionLine(); testRejectLibavcodec();
    testRejectNegative(); testRejectEmpty();
    testRejectNoPtsTime(); testRejectPtsTimeOnly();
    testRejectPtsTimeSign(); testRejectInf(); testRejectNan();
    testRejectRandomLine();

    std::printf("\n--- D: seek behavior (backward PTS accepted) ---\n");
    testSeekForward(); testSeekBackward(); testNonMonotonic();

    std::printf("\n--- E: Jellyfin ticks from parsed PTS ---\n");
    testTicksFromPts21(); testTicksFromPts1();

    std::printf("\n--- F: record extraction (CR/LF) ---\n");
    testExtractLF(); testExtractCR(); testExtractCRLF(); testExtractMixed();
    testExtractIncomplete(); testExtractFromOffset();

    std::printf("\n--- G: pts_event lifecycle (start-once bug fix) ---\n");
    testPtsEventFirstReturnsStart(); testPtsEventSecondReturnsProgress();
    testPtsEventAlwaysStartThenProgress();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B5f3b playback reporter tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
