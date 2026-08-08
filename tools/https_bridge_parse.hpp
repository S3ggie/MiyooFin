// tools/https_bridge_parse.hpp — Parsing helpers for https_bridge
// Extracted so they can be unit-tested independently.
#ifndef HTTPS_BRIDGE_PARSE_HPP
#define HTTPS_BRIDGE_PARSE_HPP

#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>

// Maximum size for incoming HTTP headers.
static constexpr size_t HTTPS_BRIDGE_MAX_HEADER_SIZE = 8192;

// Represents a parsed HTTP request from FFplay.
struct HttpRequest {
    std::string method;    // "GET" or "HEAD"
    std::string path;      // e.g. "/stream"
    std::string range;     // raw Range value, e.g. "bytes=12345-" (empty if absent)
    bool valid = false;
};

// Case-insensitive search for a header name in raw header text.
// Returns the value (trimmed) or empty string if not found.
inline std::string find_header_ci(const std::string &headers, const char *name)
{
    size_t name_len = std::strlen(name);
    size_t pos = 0;
    while (pos < headers.size()) {
        size_t eol = headers.find('\n', pos);
        if (eol == std::string::npos) eol = headers.size();

        size_t line_len = eol - pos;
        if (line_len >= name_len) {
            bool match = true;
            for (size_t i = 0; i < name_len; ++i) {
                char a = headers[pos + i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z') a += 32;
                if (b >= 'A' && b <= 'Z') b += 32;
                if (a != b) { match = false; break; }
            }
            if (match && headers[pos + name_len] == ':') {
                size_t val_start = pos + name_len + 1;
                while (val_start < eol && headers[val_start] == ' ') ++val_start;
                size_t val_end = eol;
                if (val_end > val_start && headers[val_end - 1] == '\r') --val_end;
                return headers.substr(val_start, val_end - val_start);
            }
        }
        pos = eol + 1;
    }
    return {};
}

// Parse the range spec from a Range header value like "bytes=100-200".
// Returns the spec part (e.g. "100-200" or "100-") or empty on error.
inline std::string parse_range_spec(const std::string &range_value)
{
    const char *prefix = "bytes=";
    size_t plen = std::strlen(prefix);
    if (range_value.size() < plen + 1) return {};
    for (size_t i = 0; i < plen; ++i) {
        char a = range_value[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != prefix[i]) return {};
    }
    std::string spec = range_value.substr(plen);
    size_t start = spec.find_first_not_of(' ');
    if (start == std::string::npos) return {};
    spec = spec.substr(start);
    if (spec.empty()) return {};
    bool has_digit = false;
    for (char c : spec) {
        if (c >= '0' && c <= '9') { has_digit = true; break; }
    }
    if (!has_digit) return {};
    return spec;
}

// Parse a raw HTTP request header block.
inline HttpRequest parse_request(const char *raw, size_t len)
{
    HttpRequest req;
    if (len == 0) return req;

    std::string data(raw, len);
    size_t eol = data.find('\n');
    if (eol == std::string::npos) return req;

    std::string line = data.substr(0, eol);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    size_t sp1 = line.find(' ');
    if (sp1 == std::string::npos) return req;
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return req;

    req.method = line.substr(0, sp1);
    req.path = line.substr(sp1 + 1, sp2 - sp1 - 1);

    std::transform(req.method.begin(), req.method.end(), req.method.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    std::string headers = data.substr(eol + 1);
    req.range = find_header_ci(headers, "Range");
    req.valid = true;
    return req;
}

// ===================================================================
// Upstream HTTP response parsing helpers
// ===================================================================

// Parse an HTTP status line like "HTTP/1.1 200 OK\r\n".
// Returns the 3-digit status code (e.g. 200) or 0 on parse error.
inline long parse_status_line(const char *line, size_t len)
{
    // Minimum: "HTTP/x.x 000" = 12 chars
    if (len < 12) return 0;
    if (line[0] != 'H' || line[1] != 'T' || line[2] != 'T' ||
        line[3] != 'P' || line[4] != '/') return 0;

    // Find space after version token
    size_t sp = 0;
    for (size_t i = 5; i < len; ++i) {
        if (line[i] == ' ') { sp = i; break; }
    }
    if (sp == 0 || sp + 4 > len) return 0;

    // Three-digit status code follows the space
    char a = line[sp + 1], b = line[sp + 2], c = line[sp + 3];
    if (a < '1' || a > '5') return 0;
    if (b < '0' || b > '9') return 0;
    if (c < '0' || c > '9') return 0;

    return static_cast<long>((a - '0') * 100 + (b - '0') * 10 + (c - '0'));
}

// True for HTTP redirect status codes that libcurl may follow.
inline bool is_redirect_status(long code)
{
    return code == 301 || code == 302 || code == 303 ||
           code == 307 || code == 308;
}

// True for HTTP status codes that indicate a successful media response.
inline bool is_media_status(long code)
{
    return code == 200 || code == 206;
}

// Standard reason phrases for status codes used by the bridge.
inline const char *status_reason(long code)
{
    switch (code) {
        case 200: return "OK";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 413: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        default:  return "Unknown";
    }
}

// Extract the header name from a raw header line like "Content-Type: video/mp4\r\n".
// Returns empty if no colon is found.
inline std::string extract_header_name(const char *line, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        if (line[i] == ':') return std::string(line, i);
    }
    return {};
}

// Extract the header value from a raw header line like "Content-Type: video/mp4\r\n".
// Trims leading space after colon and trailing \r\n.
inline std::string extract_header_value(const char *line, size_t len)
{
    size_t colon = 0;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] == ':') { colon = i; break; }
    }
    if (colon == 0 || colon + 1 >= len) return {};
    size_t vs = colon + 1;
    while (vs < len && line[vs] == ' ') ++vs;
    size_t ve = len;
    while (ve > vs && (line[ve - 1] == '\r' || line[ve - 1] == '\n')) --ve;
    return std::string(line + vs, ve - vs);
}

// Returns true if the header name is in the small allowlist that the bridge
// relays to the local FFplay client.
//
// Allowed: Content-Type, Content-Length, Content-Range, Accept-Ranges.
// Everything else (Location, Transfer-Encoding, Server, Date, etc.) is dropped.
inline bool is_allowed_response_header(const char *name, size_t name_len)
{
    auto ci_eq = [](const char *a, size_t alen, const char *b) -> bool {
        size_t blen = std::strlen(b);
        if (alen != blen) return false;
        for (size_t i = 0; i < alen; ++i) {
            char ca = a[i], cb = b[i];
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) return false;
        }
        return true;
    };
    return ci_eq(name, name_len, "Content-Type") ||
           ci_eq(name, name_len, "Content-Length") ||
           ci_eq(name, name_len, "Content-Range") ||
           ci_eq(name, name_len, "Accept-Ranges");
}

#endif // HTTPS_BRIDGE_PARSE_HPP
