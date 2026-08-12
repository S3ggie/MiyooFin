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
#include "playback_route.hpp"
#include "../include/miyoofin/version.hpp"

using namespace miyoofin;

// ===================================================================
// Constants
// ===================================================================

static const int POLL_INTERVAL_US    = 80000; // 80 ms
static const int HTTP_TIMEOUT_SEC    = 5;
static const size_t MAX_PARTIAL_BUF  = 4096;

// ===================================================================
// Signal handling
// ===================================================================

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) { g_running = 0; }

// ===================================================================
// Diagnostics logging
// ===================================================================

static FILE *g_logFile = nullptr;

static void reporter_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_logFile) {
        time_t now = time(nullptr);
        struct tm tm_buf;
        struct tm *tm = localtime_r(&now, &tm_buf);
        fprintf(g_logFile, "[%02d:%02d:%02d] [PlaybackReporter] ",
                tm->tm_hour, tm->tm_min, tm->tm_sec);
        vfprintf(g_logFile, fmt, ap);
        fprintf(g_logFile, "\n");
        fflush(g_logFile);
    }
    va_end(ap);
}

// ===================================================================
// File helpers
// ===================================================================

static std::string read_file(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    std::string result;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        result.append(buf, n);
    std::fclose(f);
    return result;
}

// Read a file as raw bytes (binary-safe for pos.cfg and similar)
static std::string read_file_binary(const std::string &path)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::string result;
    char buf[256];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        result.append(buf, n);
    std::fclose(f);
    return result;
}

static bool file_exists(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool nonempty_file(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static bool write_playback_result(const std::string &path,
                                  const std::string &itemId,
                                  const std::string &itemType, int64_t positionTicks, int64_t baseTicks,
                                  const std::string &sourceMode, bool serverReported)
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) return false;
    const bool wrote = std::fprintf(f, "item_id=%s\nitem_type=%s\nposition_ticks=%lld\nbase_resume_ticks=%lld\nsource_mode=%s\nserver_reported=%d\n", itemId.c_str(), itemType.c_str(), (long long)positionTicks, (long long)baseTicks, sourceMode.c_str(), serverReported?1:0) >= 0;
    const bool closed = std::fclose(f) == 0;
    return wrote && closed;
}

static std::string read_kv_from_content(const std::string &content,
                                        const char *key)
{
    std::string needle = std::string(key) + "=";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    size_t end = content.find('\n', pos);
    if (end == std::string::npos) end = content.size();
    while (end > pos && content[end - 1] == '\r') --end;
    return content.substr(pos, end - pos);
}

// ===================================================================
// libcurl write callback (discard response body)
// ===================================================================

static size_t discard_write(void *, size_t size, size_t nmemb, void *)
{
    return size * nmemb;
}

// ===================================================================
// HTTP POST helper
// ===================================================================

struct PostResult { long httpStatus=0; bool transportFailure=false; };

static PostResult post_json(const std::string &url,
                      const std::vector<std::string> &headers,
                      const std::string &body,
                      const std::string &cacertPath)
{
    CURL *curl = curl_easy_init();
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

    struct curl_slist *hdrList = nullptr;
    hdrList = curl_slist_append(hdrList, "Content-Type: application/json");
    for (const auto &h : headers)
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

static void build_identity_headers(const std::string &deviceId,
                                   std::vector<std::string> &headers)
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

static std::string build_playing_payload(const std::string &itemId,
                                         int64_t positionTicks,
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

static std::string build_stopped_payload(const std::string &itemId,
                                         int64_t positionTicks,
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

static bool report_event(const char *name, const std::string &path,
                         const PlaybackRoute &route,
                         const std::string &itemId,
                         int64_t positionTicks,
                         bool stopped, bool failed,
                         const std::string &token,
                         const std::string &deviceId,
                         const std::string &cacertPath)
{
    std::vector<std::string> headers;
    headers.push_back("X-Emby-Token: " + token);
    build_identity_headers(deviceId, headers);
    const std::string body = stopped ? build_stopped_payload(itemId, positionTicks, failed)
                                     : build_playing_payload(itemId, positionTicks, true);
    PostResult result=post_json(route.primary + path, headers, body, cacertPath);
    if (!route.fallback.empty() && playback_should_fallback(result.transportFailure, result.httpStatus)) {
        reporter_log("[ReporterRoute] LAN failed; PUBLIC");
        result=post_json(route.fallback + path, headers, body, cacertPath);
    }
    reporter_log("%s %lld ticks HTTP %ld", name, (long long)positionTicks, result.httpStatus);
    return result.httpStatus >= 200 && result.httpStatus < 300;
}

// ===================================================================
// Main
// ===================================================================

int main(int argc, char *argv[])
{
    if (argc != 2) {
        std::fprintf(stderr,
            "Usage: %s <app-dir>\n"
            "Jellyfin playback reporter for MiyooFin.\n",
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

    // Read session.txt
    std::string sessionContent = read_file(appDir + "/session.txt");
    if (sessionContent.empty()) {
        reporter_log("ERROR: cannot read session.txt");
        std::fclose(g_logFile); return 1;
    }
    std::string serverUrl   = read_kv_from_content(sessionContent, "server_url");
    std::string localServerUrl = read_kv_from_content(sessionContent, "local_server_url");
    std::string publicServerUrl = read_kv_from_content(sessionContent, "public_server_url");
    std::string accessToken = read_kv_from_content(sessionContent, "access_token");
    std::string userId      = read_kv_from_content(sessionContent, "user_id");
    std::string deviceId    = read_kv_from_content(sessionContent, "device_id");
    if (serverUrl.empty() || accessToken.empty()) {
        reporter_log("ERROR: missing server_url or access_token");
        std::fclose(g_logFile); return 1;
    }

    // Read playback-request.txt
    std::string reqContent = read_file(appDir + "/playback-request.txt");
    if (reqContent.empty()) {
        reporter_log("ERROR: cannot read playback-request.txt");
        std::fclose(g_logFile); return 1;
    }
    std::string itemId = read_kv_from_content(reqContent, "item_id");
    std::string itemType = read_kv_from_content(reqContent, "item_type");
    std::string sourceMode = read_kv_from_content(reqContent, "source_mode");
    if (itemId.empty()) {
        reporter_log("ERROR: missing item_id");
        std::fclose(g_logFile); return 1;
    }
    int64_t resumeTicks = parse_resume_ticks(
        read_kv_from_content(reqContent, "resume_ticks"));
    // Downloaded/local playback retains its established public-only reporter
    // behavior. LAN route selection belongs only to remote Jellyfin playback.
    const std::string publicRoute=publicServerUrl.empty() ? serverUrl : publicServerUrl;
    const std::string lanRoute=localServerUrl.empty() && !publicServerUrl.empty() ? serverUrl : localServerUrl;
    PlaybackRoute route=playback_route(publicRoute, sourceMode == "local" ? "" : lanRoute);
    reporter_log("item=%s server=%s", itemId.c_str(), serverUrl.c_str());
    reporter_log("[ReporterRoute] %s", route.usingLan ? "LAN" : "PUBLIC");
    reporter_log("resume ticks=%lld", (long long)resumeTicks);

    std::string cacertPath = appDir + "/cacert.pem";
    const bool reportsHttps=route.primary.compare(0,8,"https://")==0 ||
                            route.fallback.compare(0,8,"https://")==0;
    if (!nonempty_file(cacertPath)) {
        if (reportsHttps) {
            reporter_log("ERROR: cacert.pem not found for HTTPS reporting");
            std::fclose(g_logFile); return 1;
        }
        // HTTP-only LAN reporting does not use CA verification.
        cacertPath.clear();
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::string exitFilePath = appDir + "/playback-ffplay-exit.txt";
    std::remove(exitFilePath.c_str());

    // State
    std::string ffplayLog = appDir + "/playback-ffplay.log";
    FILE *logFp = nullptr;
    size_t fileOffset = 0;
    std::string partialBuf;
    double lastPts = -1.0;
    bool startAttempted = false;
    bool hasPts = false;
    bool ffplayExited = false;
    int exitCode = 0;

    reporter_log("waiting for showinfo pts_time in %s", ffplayLog.c_str());

    // Main loop: follow the growing FFplay log
    while (g_running && !ffplayExited) {
        if (file_exists(exitFilePath)) {
            ffplayExited = true;
            std::string exitContent = read_file(exitFilePath);
            if (!exitContent.empty()) exitCode = std::atoi(exitContent.c_str());
            reporter_log("FFplay exit code=%d", exitCode);
        }

        if (!logFp) logFp = std::fopen(ffplayLog.c_str(), "r");

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
                    double pts = 0.0;
                    if (parse_showinfo_pts(record, pts)) {
                        lastPts = pts;
                        hasPts = true;
                        if (!pts_event(startAttempted)) {
                            // First valid PTS → send PlaybackStart (once)
                            reporter_log("first pts %.4f sec", pts);
                            report_event("ReportPlaybackStart", "/Sessions/Playing", route, itemId,
                                         absolute_position_ticks(resumeTicks, pts), false, false,
                                         accessToken, deviceId, cacertPath);
                        } else {
                            // Subsequent valid PTS → send PlaybackProgress
                            report_event("progress", "/Sessions/Playing/Progress", route, itemId,
                                         absolute_position_ticks(resumeTicks, pts), false, false,
                                         accessToken, deviceId, cacertPath);
                        }
                    }
                }
                if (parsePos > 0) {
                    partialBuf = partialBuf.substr(parsePos);
                    if (partialBuf.size() > MAX_PARTIAL_BUF)
                        partialBuf = partialBuf.substr(
                            partialBuf.size() - MAX_PARTIAL_BUF);
                }
            }
        }

        if (ffplayExited && logFp) { std::fclose(logFp); logFp = nullptr; }
        if (!ffplayExited) usleep(POLL_INTERVAL_US);
    }

    // Drain remaining log bytes after FFplay exits
    if (!logFp) logFp = std::fopen(ffplayLog.c_str(), "r");
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
                    double pts = 0.0;
                    if (parse_showinfo_pts(record, pts)) {
                        lastPts = pts; hasPts = true;
                    }
                }
            }
        }
        std::fclose(logFp); logFp = nullptr;
    }

    // Parse any remaining partial buffer (trailing data without delimiter)
    if (!partialBuf.empty()) {
        double pts = 0.0;
        if (parse_showinfo_pts(partialBuf, pts)) {
            lastPts = pts; hasPts = true;
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
    static const char *POS_CFG_PATH =
        "/mnt/SDCARD/.tmp_update/pos.cfg";
    const char *streamKey = sourceMode == "local"
        ? "http://127.0.0.1:18080/local.m3u8"
        : "http://127.0.0.1:18080/stream";

    bool usedPosCfg = false;
    if (hasPts) {
        std::string posData = read_file_binary(POS_CFG_PATH);
        if (!posData.empty()) {
            uint32_t posSec = 0;
            if (parse_pos_cfg_position(posData, streamKey, posSec)) {
                reporter_log("pos.cfg final position=%us", (unsigned)posSec);
                lastPts = static_cast<double>(posSec);
                usedPosCfg = true;
            } else {
                reporter_log("pos.cfg position unavailable; using last PTS");
            }
        } else {
            reporter_log("pos.cfg not readable; using last PTS");
        }
        (void)usedPosCfg;  // suppress unused-variable warning in non-debug builds
    }

    // Send ReportPlaybackStopped
    bool failed = (exitCode != 0);
    if (hasPts) {
        const int64_t finalTicks =
            absolute_position_ticks(resumeTicks, lastPts);
        const std::string resultPath = appDir + "/playback-result.txt";
        const bool serverReported=report_event("stopped", "/Sessions/Playing/Stopped", route, itemId, finalTicks, true, failed, accessToken, deviceId, cacertPath);
        if (write_playback_result(resultPath, itemId, itemType, finalTicks, resumeTicks, sourceMode, serverReported))
            reporter_log("playback result position=%lld",
                         (long long)finalTicks);
        else
            reporter_log("ERROR: failed to write playback result");
    } else {
        reporter_log("no pts observed, nothing to report");
    }

    curl_global_cleanup();
    reporter_log("Reporter exiting");
    if (g_logFile) { std::fclose(g_logFile); g_logFile = nullptr; }
    return 0;
}
