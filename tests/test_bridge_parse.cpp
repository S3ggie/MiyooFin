// tests/test_bridge_parse.cpp -- Unit tests for https_bridge parsing helpers
// B5f1: Range header parsing, case-insensitive header lookup, request line
// parsing, and response status-line / header-allowlist parsing.
// All tests are pure logic (no network calls).
#include <cstdio>
#include <cstring>
#include <string>
#include "../tools/https_bridge_parse.hpp"

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
// parse_range_spec tests
// -------------------------------------------------------------------
static void testRangeSpecFull()
{
    std::printf("[test] parse_range_spec: bytes=100-200\n");
    CHECK_EQ(parse_range_spec("bytes=100-200"), "100-200");
    std::printf("[test] parse_range_spec: bytes=100-200 OK\n");
}

static void testRangeSpecOpenEnd()
{
    std::printf("[test] parse_range_spec: bytes=100-\n");
    CHECK_EQ(parse_range_spec("bytes=100-"), "100-");
    std::printf("[test] parse_range_spec: bytes=100- OK\n");
}

static void testRangeSpecZero()
{
    std::printf("[test] parse_range_spec: bytes=0-\n");
    CHECK_EQ(parse_range_spec("bytes=0-"), "0-");
    std::printf("[test] parse_range_spec: bytes=0- OK\n");
}

static void testRangeSpecLarge()
{
    std::printf("[test] parse_range_spec: bytes=12345-67890\n");
    CHECK_EQ(parse_range_spec("bytes=12345-67890"), "12345-67890");
    std::printf("[test] parse_range_spec: bytes=12345-67890 OK\n");
}

static void testRangeSpecSuffixLength()
{
    std::printf("[test] parse_range_spec: bytes=-500\n");
    CHECK_EQ(parse_range_spec("bytes=-500"), "-500");
    std::printf("[test] parse_range_spec: bytes=-500 OK\n");
}

static void testRangeSpecWithSpaces()
{
    std::printf("[test] parse_range_spec: bytes= 100-200\n");
    CHECK_EQ(parse_range_spec("bytes= 100-200"), "100-200");
    std::printf("[test] parse_range_spec: bytes= 100-200 OK\n");
}

static void testRangeSpecCaseInsensitive()
{
    std::printf("[test] parse_range_spec: Bytes=100-200\n");
    CHECK_EQ(parse_range_spec("Bytes=100-200"), "100-200");
    CHECK_EQ(parse_range_spec("BYTES=100-200"), "100-200");
    std::printf("[test] parse_range_spec: case-insensitive OK\n");
}

static void testRangeSpecInvalid()
{
    std::printf("[test] parse_range_spec: invalid inputs\n");
    CHECK(parse_range_spec("").empty());
    CHECK(parse_range_spec("nope").empty());
    CHECK(parse_range_spec("bytes=").empty());
    CHECK(parse_range_spec("bytes=abc").empty());
    CHECK(parse_range_spec("byt=").empty());
    std::printf("[test] parse_range_spec: invalid OK\n");
}

// -------------------------------------------------------------------
// find_header_ci tests
// -------------------------------------------------------------------
static void testFindHeaderBasic()
{
    std::printf("[test] find_header_ci: basic lookup\n");
    std::string hdrs = "Host: example.com\r\nRange: bytes=0-\r\n\r\n";
    CHECK_EQ(find_header_ci(hdrs, "Range"), "bytes=0-");
    CHECK_EQ(find_header_ci(hdrs, "Host"), "example.com");
    std::printf("[test] find_header_ci: basic OK\n");
}

static void testFindHeaderCaseInsensitive()
{
    std::printf("[test] find_header_ci: case-insensitive\n");
    std::string hdrs = "range: bytes=100-\r\n";
    CHECK_EQ(find_header_ci(hdrs, "Range"), "bytes=100-");
    CHECK_EQ(find_header_ci(hdrs, "RANGE"), "bytes=100-");
    CHECK_EQ(find_header_ci(hdrs, "range"), "bytes=100-");
    CHECK_EQ(find_header_ci(hdrs, "Range"), "bytes=100-");
    std::printf("[test] find_header_ci: case-insensitive OK\n");
}

static void testFindHeaderMissing()
{
    std::printf("[test] find_header_ci: missing header\n");
    std::string hdrs = "Host: example.com\r\n\r\n";
    CHECK(find_header_ci(hdrs, "Range").empty());
    CHECK(find_header_ci(hdrs, "Content-Type").empty());
    std::printf("[test] find_header_ci: missing OK\n");
}

static void testFindHeaderEmptyHeaders()
{
    std::printf("[test] find_header_ci: empty string\n");
    CHECK(find_header_ci("", "Range").empty());
    std::printf("[test] find_header_ci: empty OK\n");
}

static void testFindHeaderNoColon()
{
    std::printf("[test] find_header_ci: malformed line without colon\n");
    std::string hdrs = "Range bytes=0-\r\n";
    CHECK(find_header_ci(hdrs, "Range").empty());
    std::printf("[test] find_header_ci: no-colon OK\n");
}

// -------------------------------------------------------------------
// parse_request tests
// -------------------------------------------------------------------
static void testParseGet()
{
    std::printf("[test] parse_request: GET /stream\n");
    const char *raw = "GET /stream HTTP/1.1\r\n"
                      "Host: 127.0.0.1:18080\r\n"
                      "\r\n";
    HttpRequest req = parse_request(raw, std::strlen(raw));
    CHECK(req.valid);
    CHECK_EQ(req.method, "GET");
    CHECK_EQ(req.path, "/stream");
    CHECK(req.range.empty());
    std::printf("[test] parse_request: GET /stream OK\n");
}

static void testParseHead()
{
    std::printf("[test] parse_request: HEAD /stream\n");
    const char *raw = "HEAD /stream HTTP/1.1\r\n"
                      "Host: 127.0.0.1:18080\r\n"
                      "\r\n";
    HttpRequest req = parse_request(raw, std::strlen(raw));
    CHECK(req.valid);
    CHECK_EQ(req.method, "HEAD");
    CHECK_EQ(req.path, "/stream");
    std::printf("[test] parse_request: HEAD /stream OK\n");
}

static void testParseGetWithRange()
{
    std::printf("[test] parse_request: GET with Range\n");
    const char *raw = "GET /stream HTTP/1.1\r\n"
                      "Range: bytes=12345-\r\n"
                      "Host: 127.0.0.1:18080\r\n"
                      "\r\n";
    HttpRequest req = parse_request(raw, std::strlen(raw));
    CHECK(req.valid);
    CHECK_EQ(req.method, "GET");
    CHECK_EQ(req.path, "/stream");
    CHECK_EQ(req.range, "bytes=12345-");
    std::printf("[test] parse_request: GET with Range OK\n");
}

static void testParseGetWithRangeCaseInsensitive()
{
    std::printf("[test] parse_request: Range header case-insensitive\n");
    const char *raw = "GET /stream HTTP/1.1\r\n"
                      "range: bytes=100-200\r\n"
                      "Host: 127.0.0.1\r\n"
                      "\r\n";
    HttpRequest req = parse_request(raw, std::strlen(raw));
    CHECK(req.valid);
    CHECK_EQ(req.range, "bytes=100-200");

    const char *raw2 = "GET /stream HTTP/1.1\r\n"
                       "RANGE: bytes=500-\r\n"
                       "Host: 127.0.0.1\r\n"
                       "\r\n";
    HttpRequest req2 = parse_request(raw2, std::strlen(raw2));
    CHECK(req2.valid);
    CHECK_EQ(req2.range, "bytes=500-");
    std::printf("[test] parse_request: Range case-insensitive OK\n");
}

static void testParseInvalid()
{
    std::printf("[test] parse_request: invalid input\n");
    CHECK(!parse_request("", 0).valid);
    CHECK(!parse_request("garbage", 7).valid);
    std::printf("[test] parse_request: invalid OK\n");
}

static void testParseMethodLowercase()
{
    std::printf("[test] parse_request: lowercase method uppercased\n");
    const char *raw = "get /stream HTTP/1.1\r\n\r\n";
    HttpRequest req = parse_request(raw, std::strlen(raw));
    CHECK(req.valid);
    CHECK_EQ(req.method, "GET");
    std::printf("[test] parse_request: lowercase method OK\n");
}

// -------------------------------------------------------------------
// parse_status_line tests
// -------------------------------------------------------------------
static void testStatusLine301()
{
    std::printf("[test] parse_status_line: HTTP/1.1 301\n");
    const char *line = "HTTP/1.1 301 Moved Permanently\r\n";
    CHECK(parse_status_line(line, std::strlen(line)) == 301);
    std::printf("[test] parse_status_line: 301 OK\n");
}

static void testStatusLine200()
{
    std::printf("[test] parse_status_line: HTTP/1.1 200\n");
    const char *line = "HTTP/1.1 200 OK\r\n";
    CHECK(parse_status_line(line, std::strlen(line)) == 200);
    std::printf("[test] parse_status_line: 200 OK\n");
}

static void testStatusLine206()
{
    std::printf("[test] parse_status_line: HTTP/1.1 206\n");
    const char *line = "HTTP/1.1 206 Partial Content\r\n";
    CHECK(parse_status_line(line, std::strlen(line)) == 206);
    std::printf("[test] parse_status_line: 206 OK\n");
}

static void testStatusLineHTTP2()
{
    std::printf("[test] parse_status_line: HTTP/2 200\n");
    const char *line = "HTTP/2 200\r\n";
    CHECK(parse_status_line(line, std::strlen(line)) == 200);
    std::printf("[test] parse_status_line: HTTP/2 200 OK\n");
}

static void testStatusLineInvalid()
{
    std::printf("[test] parse_status_line: invalid inputs\n");
    CHECK(parse_status_line("", 0) == 0);
    CHECK(parse_status_line("not http", 8) == 0);
    CHECK(parse_status_line("HTTP/1.1", 8) == 0);
    CHECK(parse_status_line("HTTP/1.1 999 bad", 17) == 0);
    std::printf("[test] parse_status_line: invalid OK\n");
}

// -------------------------------------------------------------------
// is_redirect_status / is_media_status tests
// -------------------------------------------------------------------
static void testIsRedirect()
{
    std::printf("[test] is_redirect_status\n");
    CHECK(is_redirect_status(301));
    CHECK(is_redirect_status(302));
    CHECK(is_redirect_status(303));
    CHECK(is_redirect_status(307));
    CHECK(is_redirect_status(308));
    CHECK(!is_redirect_status(200));
    CHECK(!is_redirect_status(206));
    CHECK(!is_redirect_status(404));
    CHECK(!is_redirect_status(500));
    std::printf("[test] is_redirect_status OK\n");
}

static void testIsMedia()
{
    std::printf("[test] is_media_status\n");
    CHECK(is_media_status(200));
    CHECK(is_media_status(206));
    CHECK(!is_media_status(301));
    CHECK(!is_media_status(404));
    CHECK(!is_media_status(500));
    std::printf("[test] is_media_status OK\n");
}

// -------------------------------------------------------------------
// is_allowed_response_header tests
// -------------------------------------------------------------------
static void testAllowedHeaders()
{
    std::printf("[test] is_allowed_response_header\n");
    CHECK( is_allowed_response_header("Content-Type", 12));
    CHECK( is_allowed_response_header("Content-Length", 14));
    CHECK( is_allowed_response_header("Content-Range", 13));
    CHECK( is_allowed_response_header("Accept-Ranges", 13));
    CHECK(!is_allowed_response_header("Location", 8));
    CHECK(!is_allowed_response_header("Transfer-Encoding", 17));
    CHECK(!is_allowed_response_header("Server", 6));
    CHECK(!is_allowed_response_header("Date", 4));
    CHECK(!is_allowed_response_header("Connection", 10));
    CHECK(!is_allowed_response_header("Keep-Alive", 10));
    // Case-insensitive check
    CHECK( is_allowed_response_header("content-type", 12));
    CHECK( is_allowed_response_header("CONTENT-TYPE", 12));
    std::printf("[test] is_allowed_response_header OK\n");
}

// -------------------------------------------------------------------
// extract_header_name / extract_header_value tests
// -------------------------------------------------------------------
static void testExtractHeaderName()
{
    std::printf("[test] extract_header_name\n");
    const char *line = "Content-Type: video/mp4\r\n";
    CHECK_EQ(extract_header_name(line, std::strlen(line)), "Content-Type");
    std::printf("[test] extract_header_name OK\n");
}

static void testExtractHeaderValue()
{
    std::printf("[test] extract_header_value\n");
    const char *line = "Content-Type: video/mp4\r\n";
    CHECK_EQ(extract_header_value(line, std::strlen(line)), "video/mp4");
    const char *line2 = "Content-Range: bytes 0-12345/12346\r\n";
    CHECK_EQ(extract_header_value(line2, std::strlen(line2)), "bytes 0-12345/12346");
    const char *line3 = "Accept-Ranges: bytes\r\n";
    CHECK_EQ(extract_header_value(line3, std::strlen(line3)), "bytes");
    std::printf("[test] extract_header_value OK\n");
}

static void testStatusReason()
{
    std::printf("[test] status_reason\n");
    CHECK_EQ(std::string(status_reason(200)), "OK");
    CHECK_EQ(std::string(status_reason(206)), "Partial Content");
    CHECK_EQ(std::string(status_reason(301)), "Moved Permanently");
    CHECK_EQ(std::string(status_reason(404)), "Not Found");
    CHECK_EQ(std::string(status_reason(502)), "Bad Gateway");
    std::printf("[test] status_reason OK\n");
}

// -------------------------------------------------------------------
// is_allowed_response_header: full case-insensitive coverage (B5f1 fix)
// -------------------------------------------------------------------
static void testAllowedHeadersCaseInsensitive()
{
    std::printf("[test] is_allowed_response_header: full case-insensitive\n");
    CHECK( is_allowed_response_header("Content-Type", 12));
    CHECK( is_allowed_response_header("content-type", 12));
    CHECK( is_allowed_response_header("CONTENT-TYPE", 12));
    CHECK( is_allowed_response_header("CoNtEnT-TyPe", 12));
    CHECK( is_allowed_response_header("Content-Length", 14));
    CHECK( is_allowed_response_header("content-length", 14));
    CHECK( is_allowed_response_header("CONTENT-LENGTH", 14));
    CHECK( is_allowed_response_header("CoNtEnT-LeNgTh", 14));
    CHECK( is_allowed_response_header("Content-Range", 13));
    CHECK( is_allowed_response_header("content-range", 13));
    CHECK( is_allowed_response_header("CONTENT-RANGE", 13));
    CHECK( is_allowed_response_header("CoNtEnT-RaNgE", 13));
    CHECK( is_allowed_response_header("Accept-Ranges", 13));
    CHECK( is_allowed_response_header("accept-ranges", 13));
    CHECK( is_allowed_response_header("ACCEPT-RANGES", 13));
    CHECK( is_allowed_response_header("AcCePt-RaNgEs", 13));
    CHECK(!is_allowed_response_header("Location", 8));
    CHECK(!is_allowed_response_header("Transfer-Encoding", 17));
    CHECK(!is_allowed_response_header("Server", 6));
    std::printf("[test] is_allowed_response_header: full case-insensitive OK\n");
}

// -------------------------------------------------------------------
// find_header_ci: Content-Range in all case variants (B5f1 fix)
// -------------------------------------------------------------------
static void testFindHeaderContentRangeCaseInsensitive()
{
    std::printf("[test] find_header_ci: Content-Range case variants\n");
    std::string h1 = "Content-Range: bytes 100-199/1000\r\n\r\n";
    CHECK_EQ(find_header_ci(h1, "Content-Range"), "bytes 100-199/1000");
    CHECK_EQ(find_header_ci(h1, "content-range"), "bytes 100-199/1000");
    std::string h2 = "content-range: bytes 100-199/1000\r\n\r\n";
    CHECK_EQ(find_header_ci(h2, "Content-Range"), "bytes 100-199/1000");
    CHECK_EQ(find_header_ci(h2, "content-range"), "bytes 100-199/1000");
    std::string h3 = "CONTENT-RANGE: bytes 100-199/1000\r\n\r\n";
    CHECK_EQ(find_header_ci(h3, "Content-Range"), "bytes 100-199/1000");
    CHECK_EQ(find_header_ci(h3, "content-range"), "bytes 100-199/1000");
    std::string h4 = "CoNtEnT-RaNgE: bytes 100-199/1000\r\n\r\n";
    CHECK_EQ(find_header_ci(h4, "Content-Range"), "bytes 100-199/1000");
    CHECK_EQ(find_header_ci(h4, "content-range"), "bytes 100-199/1000");
    std::string h5 = "content-range: bytes 100000-100999/1881607835\r\n\r\n";
    CHECK_EQ(find_header_ci(h5, "content-range"), "bytes 100000-100999/1881607835");
    std::printf("[test] find_header_ci: Content-Range case variants OK\n");
}

// -------------------------------------------------------------------
// find_header_ci: other allowed headers case-insensitive (B5f1 fix)
// -------------------------------------------------------------------
static void testFindHeaderOtherHeadersCaseInsensitive()
{
    std::printf("[test] find_header_ci: other allowed headers case-insensitive\n");
    std::string ct1 = "content-type: video/x-matroska\r\n\r\n";
    CHECK_EQ(find_header_ci(ct1, "content-type"), "video/x-matroska");
    std::string ct2 = "CONTENT-TYPE: video/mp4\r\n\r\n";
    CHECK_EQ(find_header_ci(ct2, "content-type"), "video/mp4");
    std::string cl1 = "content-length: 1000\r\n\r\n";
    CHECK_EQ(find_header_ci(cl1, "content-length"), "1000");
    std::string cl2 = "CONTENT-LENGTH: 50000\r\n\r\n";
    CHECK_EQ(find_header_ci(cl2, "content-length"), "50000");
    std::string ar1 = "accept-ranges: bytes\r\n\r\n";
    CHECK_EQ(find_header_ci(ar1, "accept-ranges"), "bytes");
    std::string ar2 = "ACCEPT-RANGES: none\r\n\r\n";
    CHECK_EQ(find_header_ci(ar2, "accept-ranges"), "none");
    std::printf("[test] find_header_ci: other allowed headers case-insensitive OK\n");
}

// -------------------------------------------------------------------
// Case-insensitive routing simulation (B5f1 fix)
// -------------------------------------------------------------------
static void testCaseInsensitiveRouting()
{
    std::printf("[test] case-insensitive routing (header_cb logic)\n");
    auto route = [](const char *line, size_t len,
                    std::string &ct, std::string &cl,
                    std::string &cr, std::string &ar) {
        std::string name = extract_header_name(line, len);
        if (!name.empty() && is_allowed_response_header(name.c_str(), name.size())) {
            for (auto &ch : name) if (ch >= 'A' && ch <= 'Z') ch += 32;
            std::string val = extract_header_value(line, len);
            if      (name == "content-type")   ct = val;
            else if (name == "content-length")  cl = val;
            else if (name == "content-range")   cr = val;
            else if (name == "accept-ranges")   ar = val;
        }
    };
    { std::string a,b,c,d; const char *l="content-range: bytes 100000-100999/1881607835\r\n";
      route(l, std::strlen(l), a,b,c,d);
      CHECK_EQ(c, "bytes 100000-100999/1881607835"); CHECK(b.empty()); CHECK(a.empty()); CHECK(d.empty()); }
    { std::string a,b,c,d; const char *l="CONTENT-RANGE: bytes 100-199/1000\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(c, "bytes 100-199/1000"); }
    { std::string a,b,c,d; const char *l="CoNtEnT-RaNgE: bytes 100-199/1000\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(c, "bytes 100-199/1000"); }
    { std::string a,b,c,d; const char *l="Content-Range: bytes 100-199/1000\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(c, "bytes 100-199/1000"); }
    { std::string a,b,c,d; const char *l="content-type: video/x-matroska\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(a, "video/x-matroska"); }
    { std::string a,b,c,d; const char *l="content-length: 1000\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(b, "1000"); }
    { std::string a,b,c,d; const char *l="accept-ranges: bytes\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(d, "bytes"); }
    { std::string a,b,c,d; const char *l="ACCEPT-RANGES: bytes\r\n";
      route(l, std::strlen(l), a,b,c,d); CHECK_EQ(d, "bytes"); }
    std::printf("[test] case-insensitive routing (header_cb logic) OK\n");
}

// -------------------------------------------------------------------
// Content-Range value preservation (B5f1 fix)
// -------------------------------------------------------------------
static void testContentRangeValuePreservation()
{
    std::printf("[test] Content-Range value preservation\n");
    { const char *l = "content-range: bytes 100000-100999/1881607835\r\n";
      CHECK_EQ(extract_header_value(l, std::strlen(l)), "bytes 100000-100999/1881607835"); }
    { const char *l = "Content-Range: bytes 0-999/1000\r\n";
      CHECK_EQ(extract_header_value(l, std::strlen(l)), "bytes 0-999/1000"); }
    std::string hdrs = "content-range: bytes 100-199/1000\r\n\r\n";
    CHECK_EQ(find_header_ci(hdrs, "content-range"), "bytes 100-199/1000");
    std::printf("[test] Content-Range value preservation OK\n");
}

int main()
{
    std::printf("B5f1 HTTPS bridge parsing tests\n");
    std::printf("================================\n\n");

    // parse_range_spec tests
    std::printf("--- parse_range_spec tests ---\n");
    testRangeSpecFull();
    testRangeSpecOpenEnd();
    testRangeSpecZero();
    testRangeSpecLarge();
    testRangeSpecSuffixLength();
    testRangeSpecWithSpaces();
    testRangeSpecCaseInsensitive();
    testRangeSpecInvalid();

    // find_header_ci tests
    std::printf("\n--- find_header_ci tests ---\n");
    testFindHeaderBasic();
    testFindHeaderCaseInsensitive();
    testFindHeaderMissing();
    testFindHeaderEmptyHeaders();
    testFindHeaderNoColon();

    // parse_request tests
    std::printf("\n--- parse_request tests ---\n");
    testParseGet();
    testParseHead();
    testParseGetWithRange();
    testParseGetWithRangeCaseInsensitive();
    testParseInvalid();
    testParseMethodLowercase();

    // Response parsing tests (B5f1 fix)
    std::printf("\n--- parse_status_line tests ---\n");
    testStatusLine301();
    testStatusLine200();
    testStatusLine206();
    testStatusLineHTTP2();
    testStatusLineInvalid();

    std::printf("\n--- status classification tests ---\n");
    testIsRedirect();
    testIsMedia();

    std::printf("\n--- response header filtering tests ---\n");
    testAllowedHeaders();
    testAllowedHeadersCaseInsensitive();
    testFindHeaderContentRangeCaseInsensitive();
    testFindHeaderOtherHeadersCaseInsensitive();
    testExtractHeaderName();
    testExtractHeaderValue();
    testStatusReason();

    std::printf("\n--- B5f1 case-insensitive routing + Content-Range tests ---\n");
    testCaseInsensitiveRouting();
    testContentRangeValuePreservation();

    std::printf("\n");
    if (g_failures == 0) {
        std::printf("All B5f1 bridge parsing tests passed.\n");
        return 0;
    }
    std::printf("%d test(s) FAILED.\n", g_failures);
    return 1;
}
