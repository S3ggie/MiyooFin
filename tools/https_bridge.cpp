// tools/https_bridge.cpp -- Standalone HTTPS-to-HTTP streaming bridge
// B5f1 proof-of-concept: allows Onion FFplay to open a local HTTP URL
// while this helper fetches the actual media over HTTPS via libcurl.
//
// Usage: miyoofin-https-bridge <upstream-url> <cacert-path> [port]
//
// This file is intentionally kept in tools/ and is NOT linked into the
// main MiyooFin executable.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <csignal>
#include <cerrno>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <curl/curl.h>

#include "https_bridge_parse.hpp"

// ===================================================================
// Global state for signal handling
// ===================================================================
static volatile sig_atomic_t g_running = 1;
static int g_listen_fd = -1;

static void signal_handler(int /*sig*/)
{
    g_running = 0;
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}

static void log_info(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[HttpsBridge] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

// ===================================================================
// Upstream response state and streaming helpers
// ===================================================================

struct StreamContext {
    int client_fd;
    bool client_alive;
};

// Collected state for the current upstream response being parsed.
// Single-threaded -- safe as statics.
static struct {
    long status_code = 0;
    std::string content_type;
    std::string content_length;
    std::string content_range;
    std::string accept_ranges;
    bool collecting = false;        // between non-redirect status line and blank line
    bool headers_finalized = false; // blank line seen for non-redirect response
} s_upstream;

static bool s_final_headers_sent = false;
static int  s_client_fd = -1;

// ===================================================================
// send_all -- handle partial writes
// ===================================================================
static bool send_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Forward declaration -- defined below, used by send_final_headers().
static void send_error_response(long code);

// ===================================================================
// Reconstruct and send final HTTP response headers to local client.
// ===================================================================
static bool send_final_headers()
{
    // Safety: a 206 without Content-Range is invalid for single-range requests.
    // FFplay needs Content-Range to correctly seek within the media.
    // Silently accepting a 206 without Content-Range would produce a broken
    // seek experience, so we downgrade to 502 Bad Gateway.
    if (s_upstream.status_code == 206 && s_upstream.content_range.empty()) {
        log_info("WARNING: upstream returned 206 without Content-Range -- "
                 "sending 502 Bad Gateway instead");
        send_error_response(502);
        s_upstream.status_code = 502;
        return false;
    }

    std::string resp;
    resp += "HTTP/1.1 ";
    resp += std::to_string(s_upstream.status_code);
    resp += " ";
    resp += status_reason(s_upstream.status_code);
    resp += "\r\n";
    if (!s_upstream.content_type.empty())
        resp += "Content-Type: " + s_upstream.content_type + "\r\n";
    if (!s_upstream.content_length.empty())
        resp += "Content-Length: " + s_upstream.content_length + "\r\n";
    if (!s_upstream.content_range.empty())
        resp += "Content-Range: " + s_upstream.content_range + "\r\n";
    if (!s_upstream.accept_ranges.empty())
        resp += "Accept-Ranges: " + s_upstream.accept_ranges + "\r\n";
    else
        resp += "Accept-Ranges: bytes\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    return send_all(s_client_fd, resp.c_str(), resp.size());
}

// ===================================================================
// Send a clean error response for upstream failures.
// ===================================================================
static void send_error_response(long code)
{
    std::string resp;
    resp += "HTTP/1.1 ";
    resp += std::to_string(code);
    resp += " ";
    resp += status_reason(code);
    resp += "\r\n";
    resp += "Content-Length: 0\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    send_all(s_client_fd, resp.c_str(), resp.size());
}

// ===================================================================
// Libcurl write callback -- streams body to client socket.
// Redirect/error bodies are discarded; only 200/206 is streamed.
// ===================================================================
static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    auto *ctx = static_cast<StreamContext *>(ud);
    size_t total = size * nmemb;

    if (!s_final_headers_sent) {
        if (s_upstream.headers_finalized && is_media_status(s_upstream.status_code)) {
            if (!send_final_headers()) {
                ctx->client_alive = false;
                return 0;
            }
            s_final_headers_sent = true;
        } else {
            return total; // discard -- redirect/error body
        }
    }

    size_t sent = 0;
    while (sent < total) {
        ssize_t n = ::send(ctx->client_fd, ptr + sent, total - sent, MSG_NOSIGNAL);
        if (n <= 0) { ctx->client_alive = false; return 0; }
        sent += static_cast<size_t>(n);
    }
    return total;
}

// ===================================================================
// Libcurl header callback -- collects whitelist headers from the final
// upstream response.  Intermediate redirects (301 etc.) are discarded.
// ===================================================================
static size_t header_cb(char *buffer, size_t size, size_t nitems, void * /*ud*/)
{
    size_t total = size * nitems;

    // Status line: starts with "HTTP/"
    if (total > 5 && buffer[0] == 'H' && buffer[1] == 'T' &&
        buffer[2] == 'T' && buffer[3] == 'P' && buffer[4] == '/') {
        long code = parse_status_line(buffer, total);
        if (is_redirect_status(code)) {
            log_info("upstream intermediate HTTP %ld", code);
            s_upstream.collecting = false;
            s_upstream.status_code = code;
        } else {
            s_upstream.status_code = code;
            s_upstream.content_type.clear();
            s_upstream.content_length.clear();
            s_upstream.content_range.clear();
            s_upstream.accept_ranges.clear();
            s_upstream.headers_finalized = false;
            s_upstream.collecting = true;
        }
        return total;
    }

    // Blank line -- end of headers for the current response.
    if (total == 2 && buffer[0] == '\r' && buffer[1] == '\n') {
        if (s_upstream.collecting) {
            s_upstream.headers_finalized = true;
            s_upstream.collecting = false;
        }
        return total;
    }

    // Regular header -- collect into whitelist if currently collecting.
    // Header names are compared case-insensitively (HTTP spec RFC 7230 §3.2).
    // Servers such as Jellyfin may send "content-range:" in any casing, so we
    // lowercase the extracted name before routing to the correct field.
    if (s_upstream.collecting) {
        std::string name = extract_header_name(buffer, total);
        if (!name.empty() &&
            is_allowed_response_header(name.c_str(), name.size())) {
            // Normalize to lowercase for case-insensitive field routing.
            for (auto &ch : name)
                if (ch >= 'A' && ch <= 'Z') ch += 32;
            std::string val = extract_header_value(buffer, total);
            if      (name == "content-type")   s_upstream.content_type   = val;
            else if (name == "content-length")  s_upstream.content_length = val;
            else if (name == "content-range")   s_upstream.content_range  = val;
            else if (name == "accept-ranges")   s_upstream.accept_ranges  = val;
        }
    }
    return total;
}

// ===================================================================
// Handle one client connection
// ===================================================================
static void handle_client(int client_fd, const std::string &upstream_url,
                          const std::string &cacert_path)
{
    std::string req_data;
    req_data.reserve(HTTPS_BRIDGE_MAX_HEADER_SIZE);
    char buf[1024];
    bool hdrs_done = false;
    while (req_data.size() < HTTPS_BRIDGE_MAX_HEADER_SIZE) {
        ssize_t n = ::recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) return;
        req_data.append(buf, static_cast<size_t>(n));
        if (req_data.find("\r\n\r\n") != std::string::npos) { hdrs_done = true; break; }
    }
    if (!hdrs_done) {
        const char *r = "HTTP/1.1 413 Request Header Fields Too Large\r\n"
                        "Connection: close\r\n\r\n";
        send_all(client_fd, r, std::strlen(r));
        return;
    }

    HttpRequest req = parse_request(req_data.data(), req_data.size());
    if (!req.valid) {
        const char *r = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
        send_all(client_fd, r, std::strlen(r));
        return;
    }
    if ((req.method != "GET" && req.method != "HEAD") || req.path != "/stream") {
        const char *r = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n"
                        "Content-Length: 0\r\n\r\n";
        send_all(client_fd, r, std::strlen(r));
        log_info("%s %s -> 404", req.method.c_str(), req.path.c_str());
        return;
    }
    log_info("%s %s", req.method.c_str(), req.path.c_str());
    if (!req.range.empty()) log_info("Range: %s", req.range.c_str());

    CURL *curl = curl_easy_init();
    if (!curl) {
        const char *r = "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\n\r\n";
        send_all(client_fd, r, std::strlen(r));
        return;
    }

    StreamContext sctx{client_fd, true};
    curl_easy_setopt(curl, CURLOPT_URL, upstream_url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cacert_path.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, cacert_path.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sctx);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &sctx);

    // Reset state for this request (single-threaded)
    s_upstream = {};
    s_final_headers_sent = false;
    s_client_fd = client_fd;

    struct curl_slist *hdr_list = nullptr;
    if (!req.range.empty()) {
        std::string rh = "Range: " + req.range;
        hdr_list = curl_slist_append(hdr_list, rh.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr_list);
        std::string spec = parse_range_spec(req.range);
        if (!spec.empty())
            curl_easy_setopt(curl, CURLOPT_RANGE, spec.c_str());
    }
    if (req.method == "HEAD")
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);

    CURLcode res = curl_easy_perform(curl);

    // Send final headers if not yet sent (HEAD or empty-body responses)
    if (!s_final_headers_sent) {
        if (is_media_status(s_upstream.status_code)) {
            send_final_headers();
            s_final_headers_sent = true;
        } else if (s_upstream.status_code >= 400) {
            send_error_response(s_upstream.status_code);
        } else if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
            send_error_response(502);
            s_upstream.status_code = 502;
        }
    }

    if (res != CURLE_OK && res != CURLE_WRITE_ERROR) {
        log_info("curl error: %s", curl_easy_strerror(res));
    } else {
        log_info("upstream HTTP %ld", s_upstream.status_code);
        if (s_final_headers_sent) log_info("transfer complete");
        else log_info("client disconnected");
    }
    if (hdr_list) curl_slist_free_all(hdr_list);
    curl_easy_cleanup(curl);
}

static void usage(const char *argv0)
{
    std::fprintf(stderr,
        "Usage: %s <upstream-url> <cacert-path> [port]\n"
        "\n"
        "HTTPS-to-HTTP streaming bridge for MiyooFin.\n"
        "\n"
        "Arguments:\n"
        "  upstream-url  HTTPS URL of the media to stream\n"
        "  cacert-path   path to CA certificate bundle for TLS verification\n"
        "  port          local port to listen on (default: 18080)\n"
        "\n"
        "The server binds to 127.0.0.1 only.\n",
        argv0);
}

int main(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) { usage(argv[0]); return 1; }

    std::string upstream_url = argv[1];
    std::string cacert_path  = argv[2];
    int port = 18080;
    if (argc == 4) {
        port = std::atoi(argv[3]);
        if (port <= 0 || port > 65535) {
            std::fprintf(stderr, "Error: invalid port %s\n", argv[3]);
            usage(argv[0]);
            return 1;
        }
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    struct sigaction sa = {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    g_listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        std::fprintf(stderr, "Error: socket() failed: %s\n", std::strerror(errno));
        return 1;
    }
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        std::fprintf(stderr, "Error: bind() on 127.0.0.1:%d: %s\n",
                     port, std::strerror(errno));
        ::close(g_listen_fd); curl_global_cleanup();
        return 1;
    }
    if (::listen(g_listen_fd, 1) < 0) {
        std::fprintf(stderr, "Error: listen() failed: %s\n", std::strerror(errno));
        ::close(g_listen_fd); curl_global_cleanup();
        return 1;
    }

    log_info("listening on 127.0.0.1:%d", port);

    while (g_running) {
        struct sockaddr_in ca = {};
        socklen_t cl = sizeof(ca);
        int cfd = ::accept(g_listen_fd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) { if (!g_running) break; continue; }
        handle_client(cfd, upstream_url, cacert_path);
        ::close(cfd);
    }

    if (g_listen_fd >= 0) { ::close(g_listen_fd); g_listen_fd = -1; }
    curl_global_cleanup();
    log_info("shutdown");
    return 0;
}
