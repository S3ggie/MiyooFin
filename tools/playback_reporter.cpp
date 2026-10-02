// tools/playback_reporter.cpp — Standalone Jellyfin playback reporter for MiyooFin
// B5f3b: Real-time playback reporting using FFplay showinfo sampled PTS.
//
// Usage: miyoofin-playback-reporter <app-dir>
//
// Reads from <app-dir>:
//   session.txt           — server_url, access_token, user_id, device_id
//   playback-request.txt  — item_id, item_type, resume_ticks
//   playback-ffplay.log   — FFplay showinfo output (growing file, opened by reporter
//                           before FFplay starts; FFplay appends with >>)
//   playback-ffplay-exit.txt — written by playback_runner.sh when FFplay exits
//
// Writes to <app-dir>:
//   playback-reporter.log  — reporter diagnostics
//   playback-result.txt    — final absolute Jellyfin position for the UI
//
// The reporter is a separate process from FFplay.  It watches the
// playback-ffplay.log file as it grows (never blocking FFplay's I/O)
// and sends ReportPlayback* requests to the Jellyfin server.
//
// B5f3b change: Instead of parsing A-V/M-V master-clock status (which
// Onion FFplay does not emit), we parse showinfo pts_time values from
// the sampled filtergraph.  Each sampled PTS triggers a progress report.

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <csignal>
#include <cerrno>
#include <cstdarg>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>

#include "playback_clock_parser.hpp"
#include "playback_resume.hpp"
#include "../include/miyoofin/playback_tracks.hpp"
#include "playback_route.hpp"
#include "../include/miyoofin/version.hpp"

using namespace miyoofin;

// ===================================================================
// Constants
// ===================================================================

static const int POLL_INTERVAL_US = 80000; // 80 ms
static const int HTTP_TIMEOUT_SEC = 5;
static const size_t MAX_PARTIAL_BUF = 4096;

// ===================================================================
// Signal handling
// ===================================================================

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int)
{
    g_running = 0;
}

// ===================================================================
// Diagnostics logging
// ===================================================================

static FILE* g_logFile = nullptr;

static void reporter_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_logFile) {
        time_t now = time(nullptr);
        struct tm tm_buf;
        struct tm* tm = localtime_r(&now, &tm_buf);
        fprintf(g_logFile, "[%02d:%02d:%02d] [PlaybackReporter] ", tm->tm_hour, tm->tm_min,
                tm->tm_sec);
        vfprintf(g_logFile, fmt, ap);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
    }
    va_end(ap);
}

// ===================================================================
// File helpers
// ===================================================================

static std::string read_file(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return {};
    std::string result;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        result.append(buf, n);
    std::fclose(f);
    return result;
}

// Read a file as raw bytes (binary-safe for pos.cfg and similar)
static std::string read_file_binary(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return {};
    std::string result;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        result.append(buf, n);
    std::fclose(f);
    return result;
}

static bool file_exists(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool nonempty_file(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool write_playback_result(const std::string& path, const std::string& itemId,
                                  const std::string& itemType, int64_t positionTicks,
                                  int64_t baseTicks, const std::string& sourceMode,
                                  bool serverReported)
{
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return false;
    const bool wrote =
        std::fprintf(f,
                     "item_id=%s\nitem_type=%s\nposition_ticks=%lld\nbase_resume_ticks=%"
                     "lld\nsource_mode=%s\nserver_reported=%d\n",
                     itemId.c_str(), itemType.c_str(), (long long)positionTicks,
                     (long long)baseTicks, sourceMode.c_str(), serverReported ? 1 : 0) >= 0;
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
}

static std::string read_kv_from_content(const std::string& content, const char* key)
{
    std::string needle = std::string(key) + "=";
    size_t pos = content.find(needle);
    if (pos == std::string::npos)
        return {};
    pos += needle.size();
    size_t end = content.find('\n', pos);
    if (end == std::string::npos)
        end = content.size();
    while (end > pos && content[end - 1] == '\r')
        --end;
    return content.substr(pos, end - pos);
}

// ===================================================================
// libcurl write callback (discard response body)
// ===================================================================

static size_t discard_write(void*, size_t size, size_t nmemb, void*)
{
    return size * nmemb;
}

// ===================================================================
// HTTP POST helper
// ===================================================================

struct PostResult
{
    long httpStatus = 0;
    bool transportFailure = false;
};

static PostResult post_json(const std::string& url, const std::vector<std::string>& headers,
                            const std::string& body, const std::string& cacertPath)
{
    CURL* curl = curl_easy_init();
    if (!curl) {
        reporter_log("curl_easy_init failed");
        return {0, true};
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)HTTP_TIMEOUT_SEC);
    // TLS verification — MUST remain enabled
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cacertPath.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, cacertPath.c_str());

    struct curl_slist* hdrList = nullptr;
    hdrList = curl_slist_append(hdrList, "Content-Type: application/json");
    for (const auto& h : headers)
        hdrList = curl_slist_append(hdrList, h.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrList);

    char ua[128];
    std::snprintf(ua, sizeof(ua), "%s/%s", APP_NAME, VERSION_STR);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);

    CURLcode res = curl_easy_perform(curl);
    long httpStatus = 0;
    if (res != CURLE_OK)
        reporter_log("HTTP error: %s", curl_easy_strerror(res));
    else
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    curl_slist_free_all(hdrList);
    curl_easy_cleanup(curl);
    return {httpStatus, res != CURLE_OK};
}

// ===================================================================
// Jellyfin auth header builder — matches existing MiyooFin identity
// ===================================================================

static void build_identity_headers(const std::string& deviceId, std::vector<std::string>& headers)
{
    char hdr[512];
    std::snprintf(hdr, sizeof(hdr),
                  "X-Emby-Authorization: MediaBrowser "
                  "Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
                  APP_NAME, DEVICE_NAME, deviceId.c_str(), VERSION_STR);
    headers.push_back(hdr);
}

// ===================================================================
// JSON payload builders (manual — no JSON dependency)
// ===================================================================

static std::string build_playing_payload(const std::string& itemId, int64_t positionTicks,
                                         bool canSeek)
{
    std::string b = "{";
    b += "\"ItemId\":\"" + itemId + "\"";
    b += ",\"PositionTicks\":" + std::to_string(positionTicks);
    b += ",\"CanSeek\":" + std::string(canSeek ? "true" : "false");
    b += ",\"IsPaused\":false";
    b += ",\"IsMuted\":false";
    b += ",\"PlayMethod\":\"Transcode\"";
    b += ",\"RepeatMode\":\"RepeatNone\"";
    b += "}";
    return b;
}

static std::string build_stopped_payload(const std::string& itemId, int64_t positionTicks,
                                         bool failed)
{
    std::string b = "{";
    b += "\"ItemId\":\"" + itemId + "\"";
    b += ",\"PositionTicks\":" + std::to_string(positionTicks);
    b += ",\"Failed\":" + std::string(failed ? "true" : "false");
    b += "}";
    return b;
}

// ===================================================================
// Report helpers
// ===================================================================

static bool report_event(const char* name, const std::string& path, const PlaybackRoute& route,
                         const std::string& itemId, int64_t positionTicks, bool stopped,
                         bool failed, const std::string& token, const std::string& deviceId,
                         const std::string& cacertPath)
{
    std::vector<std::string> headers;
    headers.push_back("X-Emby-Token: " + token);
    build_identity_headers(deviceId, headers);
    const std::string body = stopped ? build_stopped_payload(itemId, positionTicks, failed)
                                     : build_playing_payload(itemId, positionTicks, true);
    PostResult result = post_json(route.primary + path, headers, body, cacertPath);
    if (!route.fallback.empty() &&
        playback_should_fallback(result.transportFailure, result.httpStatus)) {
        reporter_log("[ReporterRoute] LAN failed; PUBLIC");
        result = post_json(route.fallback + path, headers, body, cacertPath);
    }
    reporter_log("%s %lld ticks HTTP %ld", name, (long long)positionTicks, result.httpStatus);
    return result.httpStatus >= 200 && result.httpStatus < 300;
}

// ===================================================================
// Resume refresh (--refresh-resume)
//
// Remote playback writes resume_ticks from the screen's CACHED item, which
// is stale after the item is watched on another device. Before the player
// starts, the runner calls this mode: it asks the server for the current
// position and rewrites resume_ticks in playback-request.txt. Best effort:
// any failure keeps the cached value. Nothing sensitive is printed.
// ===================================================================

static long g_timeout_sec = 3;         // per request; raised for subtitle downloads
static size_t g_max_body = 256 * 1024; // per-request response cap; one thread only

static size_t append_write(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    std::string* out = static_cast<std::string*>(userdata);
    const size_t bytes = size * nmemb;
    if (out->size() + bytes > g_max_body)
        return 0; // oversized: abort the transfer
    out->append(static_cast<const char*>(ptr), bytes);
    return bytes;
}

struct GetResult
{
    long httpStatus = 0;
    bool transportFailure = false;
    std::string body;
};

static GetResult get_body(const std::string& url, const std::vector<std::string>& headers,
                          const std::string& cacertPath)
{
    GetResult result;
    CURL* curl = curl_easy_init();
    if (!curl) {
        result.transportFailure = true;
        return result;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, g_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    // TLS verification — MUST remain enabled
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (!cacertPath.empty())
        curl_easy_setopt(curl, CURLOPT_CAINFO, cacertPath.c_str());
    struct curl_slist* hdrList = nullptr;
    for (const auto& h : headers)
        hdrList = curl_slist_append(hdrList, h.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrList);
    char ua[128];
    std::snprintf(ua, sizeof(ua), "%s/%s", APP_NAME, VERSION_STR);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
    const CURLcode res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.httpStatus);
    result.transportFailure = res != CURLE_OK;
    curl_slist_free_all(hdrList);
    curl_easy_cleanup(curl);
    return result;
}

// Ids end up in a URL path; accept only the characters Jellyfin ids use.
static bool safe_path_id(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    for (unsigned char c : id) {
        if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-'))
            return false;
    }
    return true;
}

// Everything needed to ask the server about the item being played.
struct RemoteContext
{
    std::string requestPath, reqContent, itemId, userId;
    std::string cacertPath;
    PlaybackRoute route;
    std::vector<std::string> headers;
};

// Loads the pending remote request and session. Prints `<tag> skipped reason=...`
// and returns false when there is nothing to do (local playback, no session...).
static bool load_remote_context(const std::string& appDir, const char* tag, RemoteContext& ctx,
                                bool allowLocal = false)
{
    ctx.requestPath = appDir + "/playback-request.txt";
    ctx.reqContent = read_file(ctx.requestPath);
    if (ctx.reqContent.empty()) {
        std::printf("%s skipped reason=no_request\n", tag);
        return false;
    }
    ctx.itemId = read_kv_from_content(ctx.reqContent, "item_id");
    // Downloaded playback has no server round trips here.
    if (!allowLocal && read_kv_from_content(ctx.reqContent, "source_mode") == "local") {
        std::printf("%s skipped reason=local\n", tag);
        return false;
    }
    const std::string sessionContent = read_file(appDir + "/session.txt");
    const std::string serverUrl = read_kv_from_content(sessionContent, "server_url");
    const std::string localServerUrl = read_kv_from_content(sessionContent, "local_server_url");
    const std::string publicServerUrl = read_kv_from_content(sessionContent, "public_server_url");
    const std::string accessToken = read_kv_from_content(sessionContent, "access_token");
    ctx.userId = read_kv_from_content(sessionContent, "user_id");
    const std::string deviceId = read_kv_from_content(sessionContent, "device_id");
    if (serverUrl.empty() || accessToken.empty() || !safe_path_id(ctx.userId) ||
        !safe_path_id(ctx.itemId)) {
        std::printf("%s skipped reason=no_session_or_bad_id\n", tag);
        return false;
    }
    const std::string publicRoute = publicServerUrl.empty() ? serverUrl : publicServerUrl;
    const std::string lanRoute =
        localServerUrl.empty() && !publicServerUrl.empty() ? serverUrl : localServerUrl;
    ctx.route = playback_route(publicRoute, lanRoute);
    ctx.cacertPath = appDir + "/cacert.pem";
    const bool https = ctx.route.primary.compare(0, 8, "https://") == 0 ||
                       ctx.route.fallback.compare(0, 8, "https://") == 0;
    if (!nonempty_file(ctx.cacertPath)) {
        if (https) {
            // Never weaken TLS verification.
            std::printf("%s skipped reason=no_ca\n", tag);
            return false;
        }
        ctx.cacertPath.clear();
    }
    ctx.headers.push_back("X-Emby-Token: " + accessToken);
    build_identity_headers(deviceId, ctx.headers);
    return true;
}

// GET `path` on the primary route, falling back to the other on transport errors.
static GetResult get_with_fallback(const RemoteContext& ctx, const std::string& path)
{
    GetResult result = get_body(ctx.route.primary + path, ctx.headers, ctx.cacertPath);
    if (!ctx.route.fallback.empty() &&
        playback_should_fallback(result.transportFailure, result.httpStatus))
        result = get_body(ctx.route.fallback + path, ctx.headers, ctx.cacertPath);
    return result;
}

static int refresh_resume(const std::string& appDir)
{
    RemoteContext ctx;
    if (!load_remote_context(appDir, "resume_refresh", ctx))
        return 0;
    const std::string& reqContent = ctx.reqContent;
    const int64_t cachedTicks =
        parse_resume_ticks(read_kv_from_content(reqContent, "resume_ticks"));

    curl_global_init(CURL_GLOBAL_DEFAULT);
    GetResult result = get_with_fallback(ctx, "/Users/" + ctx.userId + "/Items/" + ctx.itemId);
    curl_global_cleanup();

    int64_t serverTicks = 0;
    const bool ok = !result.transportFailure && result.httpStatus >= 200 &&
                    result.httpStatus < 300 &&
                    playback_parse_user_position_ticks(result.body, serverTicks);
    if (!ok) {
        std::printf("resume_refresh failed http=%ld transport=%d kept=%lld\n", result.httpStatus,
                    result.transportFailure ? 1 : 0, (long long)cachedTicks);
        return 0;
    }
    if (serverTicks == cachedTicks) {
        std::printf("resume_refresh unchanged ticks=%lld\n", (long long)cachedTicks);
        return 0;
    }
    FILE* out = std::fopen(ctx.requestPath.c_str(), "w");
    const std::string updated = playback_replace_resume_ticks(reqContent, serverTicks);
    if (!out || std::fwrite(updated.data(), 1, updated.size(), out) != updated.size()) {
        if (out)
            std::fclose(out);
        std::printf("resume_refresh failed reason=write kept=%lld\n", (long long)cachedTicks);
        return 0;
    }
    std::fclose(out);
    std::printf("resume_refresh updated old=%lld new=%lld\n", (long long)cachedTicks,
                (long long)serverTicks);
    return 0;
}

// Writes `data` to `path` via a temporary name so readers never see a partial file.
static bool write_atomic(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

// --fetch-subs: write playback-tracks.txt, then save each selectable text
// subtitle track as subs/<index>.srt. Runs in the background next to the player
// (which picks files up as they appear). Best effort; nothing sensitive printed.
static int fetch_subs(const std::string& appDir)
{
    RemoteContext ctx;
    if (!load_remote_context(appDir, "subs_fetch", ctx, true))
        return 0;
    // Downloaded playback keeps its subtitles beside the downloaded segments
    // (a download has one fixed audio track, so only subtitle lines are listed).
    const bool local = read_kv_from_content(ctx.reqContent, "source_mode") == "local";
    std::string outDir = appDir;
    if (local) {
        const std::string scope = read_kv_from_content(ctx.reqContent, "download_scope");
        if (!safe_path_id(scope)) {
            std::printf("subs_fetch skipped reason=bad_scope\n");
            return 0;
        }
        outDir = appDir + "/downloads/" + scope + "/items/" + ctx.itemId;
        if (file_exists(outDir + "/playback-tracks.txt")) {
            std::printf("subs_fetch skipped reason=already_saved\n");
            return 0;
        }
    }
    const std::string tracksPath = outDir + "/playback-tracks.txt";
    const std::string subsDir = outDir + "/subs";
    std::remove(tracksPath.c_str());

    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_max_body = 512 * 1024;
    GetResult item = get_with_fallback(ctx, "/Users/" + ctx.userId + "/Items/" + ctx.itemId);
    PlaybackTracks tracks;
    if (item.transportFailure || item.httpStatus < 200 || item.httpStatus >= 300 ||
        !playback_parse_tracks(item.body, tracks) || !safe_path_id(tracks.mediaSourceId)) {
        std::printf("subs_fetch failed stage=tracks http=%ld\n", item.httpStatus);
        curl_global_cleanup();
        return 0;
    }
    ::mkdir(subsDir.c_str(), 0755);
    if (local) {
        PlaybackTracks subsOnly;
        subsOnly.mediaSourceId = tracks.mediaSourceId;
        for (const PlaybackTrack& t : tracks.tracks)
            if (t.type == 's')
                subsOnly.tracks.push_back(t);
        tracks = std::move(subsOnly);
    }
    // For a local item the list is only published once the files are in place.
    if (!local)
        write_atomic(tracksPath, playback_format_tracks(tracks));
    int saved = 0;
    // Subtitle files can be several hundred KB and the device is busy decoding:
    // allow a long transfer and one retry per track.
    g_max_body = 3 * 1024 * 1024;
    g_timeout_sec = 25;
    for (const PlaybackTrack& t : playback_subtitle_fetch_order(tracks, 12)) {
        if (!g_running)
            break;
        const std::string path = "/Videos/" + ctx.itemId + "/" + tracks.mediaSourceId +
                                 "/Subtitles/" + std::to_string(t.index) + "/0/Stream.srt";
        GetResult srt;
        for (int attempt = 0; attempt < 2 && g_running; ++attempt) {
            srt = get_with_fallback(ctx, path);
            if (!srt.transportFailure && srt.httpStatus >= 200 && srt.httpStatus < 300 &&
                !srt.body.empty())
                break;
            std::printf("subs_fetch retry track=%d http=%ld transport=%d\n", t.index,
                        srt.httpStatus, srt.transportFailure ? 1 : 0);
        }
        if (srt.transportFailure || srt.httpStatus < 200 || srt.httpStatus >= 300 ||
            srt.body.empty())
            continue;
        if (write_atomic(subsDir + "/" + std::to_string(t.index) + ".srt", srt.body))
            ++saved;
    }
    if (local && saved > 0)
        write_atomic(tracksPath, playback_format_tracks(tracks));
    curl_global_cleanup();
    std::printf("subs_fetch done tracks=%zu saved=%d\n", tracks.tracks.size(), saved);
    return 0;
}

// ===================================================================
// Main
// ===================================================================

int main(int argc, char* argv[])
{
    if (argc == 3 && std::strcmp(argv[2], "--refresh-resume") == 0)
        return refresh_resume(argv[1]);
    if (argc == 3 && std::strcmp(argv[2], "--fetch-subs") == 0) {
        std::signal(SIGTERM, signal_handler);
        return fetch_subs(argv[1]);
    }
    if (argc != 2) {
        std::fprintf(stderr,
                     "Usage: %s <app-dir> [--refresh-resume]\n"
                     "Jellyfin playback reporter for MiyooFin.\n"
                     "--refresh-resume: update resume_ticks in playback-request.txt from the\n"
                     "server (remote playback only) and exit.\n",
                     argv[0]);
        return 1;
    }

    std::string appDir = argv[1];

    struct sigaction sa = {};
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::string logPath = appDir + "/playback-reporter.log";
    g_logFile = std::fopen(logPath.c_str(), "w");
    if (!g_logFile) {
        std::fprintf(stderr, "Cannot open %s\n", logPath.c_str());
        return 1;
    }
    reporter_log("Reporter starting (app-dir=%s)", appDir.c_str());
    const time_t playbackStart = time(nullptr);

    // Read session.txt
    std::string sessionContent = read_file(appDir + "/session.txt");
    if (sessionContent.empty()) {
        reporter_log("ERROR: cannot read session.txt");
        std::fclose(g_logFile);
        return 1;
    }
    std::string serverUrl = read_kv_from_content(sessionContent, "server_url");
    std::string localServerUrl = read_kv_from_content(sessionContent, "local_server_url");
    std::string publicServerUrl = read_kv_from_content(sessionContent, "public_server_url");
    std::string accessToken = read_kv_from_content(sessionContent, "access_token");
    std::string userId = read_kv_from_content(sessionContent, "user_id");
    std::string deviceId = read_kv_from_content(sessionContent, "device_id");
    if (serverUrl.empty() || accessToken.empty()) {
        reporter_log("ERROR: missing server_url or access_token");
        std::fclose(g_logFile);
        return 1;
    }

    // Read playback-request.txt
    std::string reqContent = read_file(appDir + "/playback-request.txt");
    if (reqContent.empty()) {
        reporter_log("ERROR: cannot read playback-request.txt");
        std::fclose(g_logFile);
        return 1;
    }
    std::string itemId = read_kv_from_content(reqContent, "item_id");
    std::string itemType = read_kv_from_content(reqContent, "item_type");
    std::string sourceMode = read_kv_from_content(reqContent, "source_mode");
    if (itemId.empty()) {
        reporter_log("ERROR: missing item_id");
        std::fclose(g_logFile);
        return 1;
    }
    int64_t resumeTicks = parse_resume_ticks(read_kv_from_content(reqContent, "resume_ticks"));
    // Downloaded/local playback retains its established public-only reporter
    // behavior. LAN route selection belongs only to remote Jellyfin playback.
    const std::string publicRoute = publicServerUrl.empty() ? serverUrl : publicServerUrl;
    const std::string lanRoute =
        localServerUrl.empty() && !publicServerUrl.empty() ? serverUrl : localServerUrl;
    PlaybackRoute route = playback_route(publicRoute, sourceMode == "local" ? "" : lanRoute);
    reporter_log("item=%s server=%s", itemId.c_str(), serverUrl.c_str());
    reporter_log("[ReporterRoute] %s", route.usingLan ? "LAN" : "PUBLIC");
    reporter_log("resume ticks=%lld", (long long)resumeTicks);

    std::string cacertPath = appDir + "/cacert.pem";
    const bool reportsHttps = route.primary.compare(0, 8, "https://") == 0 ||
                              route.fallback.compare(0, 8, "https://") == 0;
    if (!nonempty_file(cacertPath)) {
        if (reportsHttps) {
            reporter_log("ERROR: cacert.pem not found for HTTPS reporting");
            std::fclose(g_logFile);
            return 1;
        }
        // HTTP-only LAN reporting does not use CA verification.
        cacertPath.clear();
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string exitFilePath = appDir + "/playback-ffplay-exit.txt";
    std::remove(exitFilePath.c_str());

    // State
    std::string ffplayLog = appDir + "/playback-ffplay.log";
    FILE* logFp = nullptr;
    size_t fileOffset = 0;
    std::string partialBuf;
    double lastPts = -1.0;
    // pts are relative to the stream's start; a player stream reopen (audio swap)
    // moves that start, announced by an MFBASE marker.
    int64_t streamBaseTicks = resumeTicks;
    bool startAttempted = false;
    bool hasPts = false;
    bool ffplayExited = false;
    int exitCode = 0;
    bool outputInitObserved = false;
    bool outputInitFailed = false;

    auto inspectPlayerOutput = [&](const std::string& record) {
        if (!outputInitFailed && player_video_output_initialization_failed(record)) {
            outputInitFailed = true;
            reporter_log("player_video_output_initialization_failed evidence=ffplay_stderr");
        } else if (!outputInitObserved && player_video_output_initialized(record)) {
            outputInitObserved = true;
            reporter_log("player_video_output_initialized evidence=ffplay_stderr");
        }
    };

    reporter_log("waiting for showinfo pts_time in %s", ffplayLog.c_str());

    // Main loop: follow the growing FFplay log
    while (g_running && !ffplayExited) {
        if (file_exists(exitFilePath)) {
            ffplayExited = true;
            std::string exitContent = read_file(exitFilePath);
            if (!exitContent.empty())
                exitCode = std::atoi(exitContent.c_str());
            reporter_log("FFplay exit code=%d", exitCode);
        }

        if (!logFp)
            logFp = std::fopen(ffplayLog.c_str(), "r");

        if (logFp) {
            long curSize = 0;
            if (std::fseek(logFp, 0, SEEK_END) == 0)
                curSize = std::ftell(logFp);
            if (curSize > (long)fileOffset) {
                std::fseek(logFp, fileOffset, SEEK_SET);
                size_t toRead = (size_t)(curSize - fileOffset);
                size_t oldLen = partialBuf.size();
                partialBuf.resize(oldLen + toRead);
                size_t nr = std::fread(&partialBuf[oldLen], 1, toRead, logFp);
                partialBuf.resize(oldLen + nr);
                fileOffset += nr;

                size_t parsePos = 0;
                std::string record;
                while (extract_record(partialBuf, parsePos, record)) {
                    inspectPlayerOutput(record);
                    {
                        long long base = 0;
                        if (parse_mfbase_ticks(record, base)) {
                            streamBaseTicks = base;
                            reporter_log("stream base ticks=%lld", base);
                        }
                    }
                    double pts = 0.0;
                    if (parse_showinfo_pts(record, pts)) {
                        lastPts = pts;
                        hasPts = true;
                        if (!pts_event(startAttempted)) {
                            // First valid PTS → send PlaybackStart (once)
                            reporter_log("first_video_frame_decoded pts=%.4f evidence=showinfo",
                                         pts);
                            reporter_log("first pts %.4f sec", pts);
                            report_event("ReportPlaybackStart", "/Sessions/Playing", route, itemId,
                                         absolute_position_ticks(streamBaseTicks, pts), false,
                                         false, accessToken, deviceId, cacertPath);
                        } else {
                            // Subsequent valid PTS → send PlaybackProgress
                            report_event("progress", "/Sessions/Playing/Progress", route, itemId,
                                         absolute_position_ticks(streamBaseTicks, pts), false,
                                         false, accessToken, deviceId, cacertPath);
                        }
                    }
                }
                if (parsePos > 0) {
                    partialBuf = partialBuf.substr(parsePos);
                    if (partialBuf.size() > MAX_PARTIAL_BUF)
                        partialBuf = partialBuf.substr(partialBuf.size() - MAX_PARTIAL_BUF);
                }
            }
        }

        if (ffplayExited && logFp) {
            std::fclose(logFp);
            logFp = nullptr;
        }
        if (!ffplayExited)
            usleep(POLL_INTERVAL_US);
    }

    // Drain remaining log bytes after FFplay exits
    if (!logFp)
        logFp = std::fopen(ffplayLog.c_str(), "r");
    if (logFp) {
        if (std::fseek(logFp, 0, SEEK_END) == 0) {
            long curSize = std::ftell(logFp);
            if (curSize > (long)fileOffset) {
                std::fseek(logFp, fileOffset, SEEK_SET);
                size_t toRead = (size_t)(curSize - fileOffset);
                size_t oldLen = partialBuf.size();
                partialBuf.resize(oldLen + toRead);
                size_t nr = std::fread(&partialBuf[oldLen], 1, toRead, logFp);
                partialBuf.resize(oldLen + nr);
                size_t parsePos = 0;
                std::string record;
                while (extract_record(partialBuf, parsePos, record)) {
                    inspectPlayerOutput(record);
                    {
                        long long base = 0;
                        if (parse_mfbase_ticks(record, base)) {
                            streamBaseTicks = base;
                            reporter_log("stream base ticks=%lld", base);
                        }
                    }
                    double pts = 0.0;
                    if (parse_showinfo_pts(record, pts)) {
                        lastPts = pts;
                        hasPts = true;
                    }
                }
            }
        }
        std::fclose(logFp);
        logFp = nullptr;
    }

    // Parse any remaining partial buffer (trailing data without delimiter)
    if (!partialBuf.empty()) {
        double pts = 0.0;
        if (parse_showinfo_pts(partialBuf, pts)) {
            lastPts = pts;
            hasPts = true;
        }
    }

    // ---- pos.cfg final-position lookup (after FFplay has exited) ----
    //
    // OnionOS FFplay saves its playback position into
    // /mnt/SDCARD/.tmp_update/pos.cfg as fixed-size 264-byte records.
    // The record whose key matches our stream URL contains the most
    // authoritative final position.  We attempt this lookup once,
    // after draining all showinfo output.  On any failure we silently
    // fall back to the latest sampled showinfo PTS — the existing
    // behaviour.
    static const char* POS_CFG_PATH = "/mnt/SDCARD/.tmp_update/pos.cfg";
    const char* streamKey = sourceMode == "local" ? "http://127.0.0.1:18080/local.m3u8"
                                                  : "http://127.0.0.1:18080/stream";

    bool usedPosCfg = false;
    if (hasPts) {
        std::string posData = read_file_binary(POS_CFG_PATH);
        if (!posData.empty()) {
            uint32_t posSec = 0;
            if (parse_pos_cfg_position(posData, streamKey, posSec)) {
                struct stat cfgStat;
                const time_t cfgMtime = stat(POS_CFG_PATH, &cfgStat) == 0 ? cfgStat.st_mtime : 0;
                if (pos_cfg_is_trustworthy(cfgMtime, playbackStart, posSec, lastPts)) {
                    reporter_log("pos.cfg final position=%us", (unsigned)posSec);
                    lastPts = static_cast<double>(posSec);
                    usedPosCfg = true;
                } else {
                    // Stale (not written this playback) or disagreeing with the
                    // player's own clock: report the sampled PTS instead.
                    reporter_log("pos.cfg position=%us rejected (stale or disagrees with "
                                 "sampled pts=%.1fs); using last PTS",
                                 (unsigned)posSec, lastPts);
                }
            } else {
                reporter_log("pos.cfg position unavailable; using last PTS");
            }
        } else {
            reporter_log("pos.cfg not readable; using last PTS");
        }
        (void)usedPosCfg; // suppress unused-variable warning in non-debug builds
    }

    if (outputInitFailed) {
        reporter_log(
            "playback_attempt_classification=failed reason=player_video_output_initialization");
    } else if (!outputInitObserved) {
        reporter_log("player_video_output_initialization=not_observed");
    }

    // Send ReportPlaybackStopped. A player output-init error is a failed
    // attempt even when FFplay itself returned zero; no retry is performed.
    bool failed = (exitCode != 0) || outputInitFailed;
    if (hasPts) {
        const int64_t finalTicks = absolute_position_ticks(streamBaseTicks, lastPts);
        const std::string resultPath = appDir + "/playback-result.txt";
        const bool serverReported =
            report_event("stopped", "/Sessions/Playing/Stopped", route, itemId, finalTicks, true,
                         failed, accessToken, deviceId, cacertPath);
        if (write_playback_result(resultPath, itemId, itemType, finalTicks, resumeTicks, sourceMode,
                                  serverReported))
            reporter_log("playback result position=%lld", (long long)finalTicks);
        else
            reporter_log("ERROR: failed to write playback result");
    } else {
        reporter_log("no pts observed, nothing to report");
    }

    curl_global_cleanup();
    reporter_log("Reporter exiting");
    if (g_logFile) {
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    return 0;
}
