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
#include "../tools/playback_route.hpp"

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

static void testPlaybackRoutes() {
    PlaybackRoute publicOnly=playback_route("https://public", "");
    CHECK_EQ(publicOnly.primary, std::string("https://public")); CHECK(publicOnly.fallback.empty()); CHECK(!publicOnly.usingLan);
    PlaybackRoute lan=playback_route("https://public", "http://lan");
    CHECK_EQ(lan.primary, std::string("http://lan")); CHECK_EQ(lan.fallback, std::string("https://public")); CHECK(lan.usingLan);
    CHECK(playback_should_fallback(true, 0));
    CHECK(!playback_should_fallback(false, 401)); CHECK(!playback_should_fallback(false, 403));
    // One failed transport attempt is retried through the fallback once: this
    // preserves one logical Start/Stopped event rather than emitting duplicates.
    int attempts=0; if (playback_should_fallback(true, 0)) ++attempts; ++attempts; CHECK(attempts==2);
}

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

// A2: resume parsing and absolute-position conversion
static void testResumeTicksParsing() {
    std::printf("[test] resume_ticks parsing\n");
    CHECK(parse_resume_ticks("") == 0);
    CHECK(parse_resume_ticks("not-a-number") == 0);
    CHECK(parse_resume_ticks("12oops") == 0);
    CHECK(parse_resume_ticks("-42") == 0);
    CHECK(parse_resume_ticks("6734640000") == 6734640000LL);
    CHECK(parse_resume_ticks("9223372036854775808") == 0);
    std::printf("[test] resume_ticks parsing OK\n");
}

static void testAbsolutePositionTicks() {
    std::printf("[test] absolute resume positions\n");
    CHECK(absolute_position_ticks(0, 18.0) == 180000000LL);
    CHECK(absolute_position_ticks(6734640000LL, 18.0) == 6914640000LL);
    CHECK(absolute_position_ticks(6734640000LL, 1.48438) == 6749483800LL);
    CHECK(absolute_position_ticks(6734640000LL, 6.48938) == 6799533800LL);
    CHECK(absolute_position_ticks(-1, 18.0) == 180000000LL);
    CHECK(add_resume_ticks(std::numeric_limits<int64_t>::max() - 5, 10)
          == std::numeric_limits<int64_t>::max());
    CHECK(add_resume_ticks(10, -1) == 10);
    std::printf("[test] absolute resume positions OK\n");
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
// H: pos.cfg parsing tests
// ================================================================

static const char *MIYOOFIN_KEY = "http://127.0.0.1:18080/stream";

// Helper: build a single 264-byte record
static std::string make_record(const char *key, uint8_t posBytes[4])
{
    std::string rec(POS_CFG_RECORD_SIZE, '\0');
    if (key) {
        size_t klen = std::strlen(key);
        if (klen > POS_CFG_KEY_SIZE) klen = POS_CFG_KEY_SIZE;
        std::memcpy(&rec[0], key, klen);
    }
    rec[POS_CFG_POS_OFFSET + 0] = (char)posBytes[0];
    rec[POS_CFG_POS_OFFSET + 1] = (char)posBytes[1];
    rec[POS_CFG_POS_OFFSET + 2] = (char)posBytes[2];
    rec[POS_CFG_POS_OFFSET + 3] = (char)posBytes[3];
    return rec;
}

// H1: One complete 264-byte record, position = 32
static void testPosCfgSingleRecord32() {
    std::printf("[test] pos.cfg: single record position=32\n");
    uint8_t pos[4] = {0x20, 0x00, 0x00, 0x00};
    std::string data = make_record(MIYOOFIN_KEY, pos);
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 32);
    std::printf("[test] pos.cfg: single record 32 OK\n");
}

// H2: Little-endian decode: 0x52 = 82 seconds
static void testPosCfgLE82() {
    std::printf("[test] pos.cfg: little-endian decode 82\n");
    uint8_t pos[4] = {0x52, 0x00, 0x00, 0x00};
    std::string data = make_record(MIYOOFIN_KEY, pos);
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 82);
    std::printf("[test] pos.cfg: LE 82 OK\n");
}

// H3: Multiple records — unrelated record followed by MiyooFin record
static void testPosCfgUnrelatedThenMiyooFin() {
    std::printf("[test] pos.cfg: unrelated record then MiyooFin\n");
    uint8_t posUnrel[4] = {0x0A, 0x00, 0x00, 0x00};
    uint8_t posMiyoo[4] = {0x16, 0x00, 0x00, 0x00};
    std::string data =
        make_record("http://192.168.1.100:8096/videos", posUnrel) +
        make_record(MIYOOFIN_KEY, posMiyoo);
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 22);
    std::printf("[test] pos.cfg: unrelated then MiyooFin OK\n");
}

// H4: MiyooFin record NOT last — still finds it
static void testPosCfgMiyooFinNotLast() {
    std::printf("[test] pos.cfg: MiyooFin record NOT last\n");
    uint8_t posMiyoo[4] = {0x15, 0x00, 0x00, 0x00};
    uint8_t posAfter[4] = {0xFF, 0x00, 0x00, 0x00};
    std::string data =
        make_record(MIYOOFIN_KEY, posMiyoo) +
        make_record("something.else", posAfter);
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 21);
    std::printf("[test] pos.cfg: MiyooFin not last OK\n");
}

// H5: Multiple exact matching records — last one wins
static void testPosCfgMultipleMatchesLastWins() {
    std::printf("[test] pos.cfg: multiple exact matches, last wins\n");
    uint8_t pos1[4] = {0x0A, 0x00, 0x00, 0x00};
    uint8_t pos2[4] = {0x20, 0x00, 0x00, 0x00};
    uint8_t pos3[4] = {0x37, 0x00, 0x00, 0x00};
    std::string data =
        make_record(MIYOOFIN_KEY, pos1) +
        make_record("distractor", pos2) +
        make_record(MIYOOFIN_KEY, pos3);
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 55);
    std::printf("[test] pos.cfg: multiple matches last wins OK\n");
}

// H6: Missing matching key — failure
static void testPosCfgMissingKey() {
    std::printf("[test] pos.cfg: missing matching key\n");
    uint8_t pos[4] = {0x0A, 0x00, 0x00, 0x00};
    std::string data = make_record("http://wrong-server:8096/stream", pos);
    uint32_t result = 99;
    CHECK(!parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 99); // unchanged
    std::printf("[test] pos.cfg: missing key OK\n");
}

// H7: Incomplete trailing record — safely ignored
static void testPosCfgIncompleteTrailing() {
    std::printf("[test] pos.cfg: incomplete trailing record\n");
    uint8_t pos[4] = {0x16, 0x00, 0x00, 0x00};
    std::string data = make_record(MIYOOFIN_KEY, pos);
    data.append(100, '\x00'); // partial record
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(data, MIYOOFIN_KEY, result));
    CHECK(result == 22);
    std::printf("[test] pos.cfg: incomplete trailing OK\n");
}

// H8: 256-byte key field without any NUL — rejected
static void testPosCfgNoNulInKeyField() {
    std::printf("[test] pos.cfg: no NUL in key field\n");
    std::string rec(POS_CFG_RECORD_SIZE, '\xFF');
    uint8_t pos[4] = {0x0A, 0x00, 0x00, 0x00};
    rec[POS_CFG_POS_OFFSET + 0] = pos[0];
    rec[POS_CFG_POS_OFFSET + 1] = pos[1];
    rec[POS_CFG_POS_OFFSET + 2] = pos[2];
    rec[POS_CFG_POS_OFFSET + 3] = pos[3];
    uint32_t result = 0;
    CHECK(!parse_pos_cfg_position(rec, MIYOOFIN_KEY, result));
    std::printf("[test] pos.cfg: no NUL rejected OK\n");
}

// H9: Stale bytes AFTER the NUL — key before NUL still matches
static void testPosCfgStaleBytesAfterNul() {
    std::printf("[test] pos.cfg: stale bytes after NUL still matches\n");
    std::string rec(POS_CFG_RECORD_SIZE, '\0');
    size_t klen = std::strlen(MIYOOFIN_KEY);
    std::memcpy(&rec[0], MIYOOFIN_KEY, klen);
    // Garbage after NUL within the 256-byte field
    for (size_t i = klen + 1; i < POS_CFG_KEY_SIZE; ++i)
        rec[i] = (char)(0xAA + (i & 0x0F));
    uint8_t pos[4] = {0x2A, 0x00, 0x00, 0x00};
    rec[POS_CFG_POS_OFFSET + 0] = pos[0];
    rec[POS_CFG_POS_OFFSET + 1] = pos[1];
    rec[POS_CFG_POS_OFFSET + 2] = pos[2];
    rec[POS_CFG_POS_OFFSET + 3] = pos[3];
    uint32_t result = 0;
    CHECK(parse_pos_cfg_position(rec, MIYOOFIN_KEY, result));
    CHECK(result == 42);
    std::printf("[test] pos.cfg: stale bytes after NUL OK\n");
}

// H10: Successful pos.cfg value overrides sampled PTS
static void testPosCfgOverridesSampledPts() {
    std::printf("[test] pos.cfg: value overrides sampled PTS\n");
    double lastPts = 31.4833;
    uint8_t posBytes[4] = {0x20, 0x00, 0x00, 0x00};
    std::string data = make_record(MIYOOFIN_KEY, posBytes);
    uint32_t posSec = 0;
    bool found = parse_pos_cfg_position(data, MIYOOFIN_KEY, posSec);
    CHECK(found);
    if (found) lastPts = static_cast<double>(posSec);
    int64_t ticks = absolute_position_ticks(6734640000LL, lastPts);
    CHECK(ticks == 7054640000LL);
    std::printf("[test] pos.cfg: overrides sampled PTS OK\n");
}

// H11: Failed pos.cfg parse falls back to latest sampled showinfo PTS
static void testPosCfgFallbackToSampledPts() {
    std::printf("[test] pos.cfg: fallback to sampled PTS\n");
    double lastPts = 27.2592;
    uint8_t posBytes[4] = {0x01, 0x00, 0x00, 0x00};
    std::string data = make_record("http://wrong-server/stream", posBytes);
    uint32_t posSec = 0;
    bool found = parse_pos_cfg_position(data, MIYOOFIN_KEY, posSec);
    CHECK(!found);
    int64_t ticks = absolute_position_ticks(6734640000LL, lastPts);
    CHECK(ticks == 7007232000LL);
    std::printf("[test] pos.cfg: fallback to sampled PTS OK\n");
}

// ================================================================
// Main
// ================================================================
int main()
{
    std::printf("B5f3b Playback Reporter Tests\n");
    std::printf("=============================\n\n");

    std::printf("--- Route selection and fallback policy ---\n");
    testPlaybackRoutes();

    std::printf("--- A: seconds_to_ticks ---\n");
    testTicksZero(); testTicks525(); testTicks60(); testTicks138742();
    testTicksNegativeClamp(); testTicksNaN();

    std::printf("\n--- A2: resume ticks and absolute positions ---\n");
    testResumeTicksParsing(); testAbsolutePositionTicks();

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

    std::printf("\n--- H: pos.cfg binary-record parsing ---\n");
    testPosCfgSingleRecord32(); testPosCfgLE82();
    testPosCfgUnrelatedThenMiyooFin(); testPosCfgMiyooFinNotLast();
    testPosCfgMultipleMatchesLastWins(); testPosCfgMissingKey();
    testPosCfgIncompleteTrailing(); testPosCfgNoNulInKeyField();
    testPosCfgStaleBytesAfterNul(); testPosCfgOverridesSampledPts();
    testPosCfgFallbackToSampledPts();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All playback reporter tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
