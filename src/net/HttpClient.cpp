#include "HttpClient.hpp"
#include "ClockCheck.hpp"
#include "TlsConfig.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include "miyoofin/version.hpp"
#include <curl/curl.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace miyoofin {

// -------------------------------------------------------------------
// libcurl write callback — appends data to a std::string.
// -------------------------------------------------------------------
static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    auto *s = static_cast<std::string *>(userp);
    s->append(static_cast<const char *>(contents), total);
    return total;
}
static int cancelCallback(void *userp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto *cancelled = static_cast<const std::atomic<bool> *>(userp);
    return cancelled && cancelled->load(std::memory_order_acquire);
}

// -------------------------------------------------------------------
// libcurl write callback for binary data — appends to a vector,
// aborting if the maximum size is exceeded.
// -------------------------------------------------------------------
struct BinaryWriteContext {
    std::vector<unsigned char> *data;
    size_t                      maxSize;
    bool                        exceeded;
};

static size_t binaryWriteCallback(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    auto *ctx = static_cast<BinaryWriteContext *>(userp);

    if (ctx->data->size() + total > ctx->maxSize) {
        ctx->exceeded = true;
        return 0;  // returning 0 aborts the transfer
    }

    size_t offset = ctx->data->size();
    ctx->data->resize(offset + total);
    std::memcpy(ctx->data->data() + offset, contents, total);
    return total;
}

/// Classify a curl TLS transport failure.  When the failure looks
/// like a certificate/peer-verification error AND the system clock
/// is obviously wrong, return the user-friendly clock message.
/// Otherwise return the default "Transport: ..." string.
static std::string classifyTransportError(CURLcode res)
{
    if (shouldShowClockError(res == CURLE_PEER_FAILED_VERIFICATION,
                             std::time(nullptr))) {
        return kClockErrorMessage;
    }
    return std::string("Transport: ") + curl_easy_strerror(res);
}

static void recordNetworkRequest(TelemetryTimer &timer, uint8_t method,
                                 long httpStatus, CURLcode curlCode,
                                 size_t rxBytes, size_t txBytes,
                                 bool cancelled, bool truncated) noexcept
{
    PerformanceTelemetry &telemetry = performanceTelemetry();
    if (!timer.active() || !telemetry.enabledFast())
        return;

    TelemetryRecord record{};
    record.header.record_type = RecordType::NetworkRequest;
    record.payload.network_request.request_kind = static_cast<uint16_t>(
        currentRequestKind());
    record.payload.network_request.route_kind = static_cast<uint8_t>(
        currentRouteKind());
    record.payload.network_request.method = method;
    record.payload.network_request.duration_us = timer.elapsedUs();
    record.payload.network_request.http_status = static_cast<uint32_t>(
        httpStatus < 0 ? 0 : httpStatus);
    record.payload.network_request.curl_code = static_cast<uint32_t>(curlCode);
    record.payload.network_request.rx_payload_bytes = static_cast<uint64_t>(rxBytes);
    record.payload.network_request.tx_body_bytes = static_cast<uint64_t>(txBytes);
    record.payload.network_request.attempt = currentRouteAttempt();
    record.payload.network_request.cancelled = cancelled ? 1 : 0;
    record.payload.network_request.truncated = truncated ? 1 : 0;
    record.payload.network_request.fallback_attempt = currentRouteFallback() ? 1 : 0;
    telemetry.emitRecord(record);
}

HttpClient::HttpClient()
{
    // Global init is handled once in main via curl_global_init
}

HttpClient::~HttpClient()
{
    if (m_curl)
        curl_easy_cleanup(m_curl);
}

bool HttpClient::get(const std::string &url,
                     std::string &responseBody,
                     long &httpCode,
                     std::string &error)
{
    HttpResponse response;
    if (!perform("GET", url, {}, {}, response, error))
        return false;

    responseBody = response.body;
    httpCode = response.status;

    if (response.status != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", response.status);
        error = buf;
        return false;
    }

    if (responseBody.empty()) {
        error = "Empty response body";
        return false;
    }

    return true;
}

bool HttpClient::post(const std::string &url,
                      const std::vector<std::string> &headers,
                      const std::string &postBody,
                      HttpResponse &response,
                      std::string &error)
{
    return perform("POST", url, headers, postBody, response, error);
}

bool HttpClient::getBinary(const std::string &url,
                           const std::vector<std::string> &headers,
                           BinaryHttpResponse &response,
                           std::string &error,
                           size_t maxSize, const std::atomic<bool> *cancelled)
{
    response.status = 0;
    response.transportCode = 0;
    response.data.clear();
    response.truncated = false;
    error.clear();

    CURL *curl = m_curl ? m_curl : (m_curl = curl_easy_init());
    if (!curl) {
        error = "Failed to initialise libcurl easy handle";
        return false;
    }

    BinaryWriteContext ctx;
    ctx.data = &response.data;
    ctx.maxSize = maxSize;
    ctx.exceeded = false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, binaryWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    if (!configureTls(curl, url, &error)) return false;
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (cancelled) { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelCallback); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); }
    else { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nullptr); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L); }

    char ua[128];
    std::snprintf(ua, sizeof(ua), "%s/%s", APP_NAME, VERSION_STR);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);

    // Custom headers
    struct curl_slist *headerList = nullptr;
    for (const auto &h : headers) {
        headerList = curl_slist_append(headerList, h.c_str());
    }
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    TelemetryTimer timer;
    CURLcode res = curl_easy_perform(curl);
    response.transportCode = static_cast<int>(res);

    if (res != CURLE_OK) {
        recordNetworkRequest(timer, 1, response.status, res,
                             response.data.size(), 0,
                             res == CURLE_ABORTED_BY_CALLBACK, ctx.exceeded);
        error = std::string("Transport: ") + curl_easy_strerror(res);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headerList);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headerList);

    if (ctx.exceeded) {
        response.truncated = true;
        response.data.clear();
    }

    recordNetworkRequest(timer, 1, response.status, res,
                         response.data.size(), 0, false, response.truncated);

    return true;
}

bool HttpClient::perform(const std::string &method,
                         const std::string &url,
                         const std::vector<std::string> &headers,
                         const std::string &postBody,
                         HttpResponse &response,
                         std::string &error, const std::atomic<bool> *cancelled)
{
    response.status = 0;
    response.transportCode = 0;
    response.body.clear();
    error.clear();

    CURL *curl = m_curl ? m_curl : (m_curl = curl_easy_init());
    if (!curl) {
        error = "Failed to initialise libcurl easy handle";
        return false;
    }

    // Configure the request
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    if (!configureTls(curl, url, &error)) return false;
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (cancelled) { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelCallback); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); }
    else { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, nullptr); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L); }

    char ua[128];
    std::snprintf(ua, sizeof(ua), "%s/%s", APP_NAME, VERSION_STR);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);

    // Custom headers (if any)
    struct curl_slist *headerList = nullptr;
    for (const auto &h : headers) {
        headerList = curl_slist_append(headerList, h.c_str());
    }

    if (!postBody.empty() || method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)postBody.size());
        if (!headerList)
            headerList = curl_slist_append(headerList, "Content-Type: application/json");
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    TelemetryTimer timer;
    CURLcode res = curl_easy_perform(curl);
    response.transportCode = static_cast<int>(res);

    if (res != CURLE_OK) {
        recordNetworkRequest(timer, method == "POST" ? 2 : 1,
                             response.status, res, response.body.size(),
                             postBody.size(), res == CURLE_ABORTED_BY_CALLBACK,
                             false);
        error = classifyTransportError(res);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headerList);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headerList);

    recordNetworkRequest(timer, method == "POST" ? 2 : 1,
                         response.status, res, response.body.size(),
                         postBody.size(), false, false);

    return true;
}

// -------------------------------------------------------------------
// downloadToFile — streaming file download with resume support
// -------------------------------------------------------------------

struct FileWriteContext {
    FILE *f;
    std::uint64_t *outBytes;
    std::uint64_t written;
};

static size_t fileWriteCallback(void *contents, size_t size, size_t nmemb,
                                void *userp)
{
    size_t total = size * nmemb;
    auto *ctx = static_cast<FileWriteContext *>(userp);
    size_t w = std::fwrite(contents, 1, total, ctx->f);
    ctx->written += w;
    return w;
}

/// Combined cancel + progress context for XFERINFOFUNCTION.
struct FileXferContext {
    const std::atomic<bool> *cancelled;
    DownloadProgress         progress;
    std::uint64_t            lastReportedMs;
    std::uint64_t            lastTotal;
};

static int fileXferCallback(void *userp, curl_off_t dltotal,
                            curl_off_t, curl_off_t dlnow, curl_off_t)
{
    auto *ctx = static_cast<FileXferContext *>(userp);

    // Cancel check
    if (ctx->cancelled && ctx->cancelled->load(std::memory_order_acquire))
        return 1;

    // Progress reporting (throttled to ~4/s)
    if (ctx->progress) {
        auto received = static_cast<std::uint64_t>(dlnow);
        auto total = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
        if (total != ctx->lastTotal)
            ctx->lastTotal = total;

        auto nowMs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        auto elapsed = nowMs - ctx->lastReportedMs;
        if (ctx->lastReportedMs == 0 || elapsed >= 250 ||
            (ctx->lastTotal > 0 && received >= ctx->lastTotal)) {
            ctx->progress(received, ctx->lastTotal);
            ctx->lastReportedMs = nowMs;
        }
    }

    return 0;
}

bool HttpClient::downloadToFile(const std::string &url,
                                const std::vector<std::string> &headers,
                                const std::string &destTmpPath,
                                std::string &error,
                                std::uint64_t *outBytes,
                                DownloadProgress progress,
                                const std::atomic<bool> *cancelled,
                                long timeoutSec,
                                long connectTimeoutSec,
                                std::uint64_t resumeFrom)
{
    error.clear();
    if (outBytes) *outBytes = 0;

    CURL *curl = m_curl ? m_curl : (m_curl = curl_easy_init());
    if (!curl) {
        error = "Failed to initialise libcurl easy handle";
        return false;
    }

    const char *mode = (resumeFrom > 0) ? "ab" : "wb";
    FILE *f = std::fopen(destTmpPath.c_str(), mode);
    if (!f) {
        error = "Failed to open output file: " + destTmpPath;
        return false;
    }

    FileWriteContext writeCtx{f, outBytes, 0};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    if (!configureTls(curl, url, &error)) {
        std::fclose(f);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);

    // Combined cancel + progress callback
    FileXferContext xferCtx{cancelled, std::move(progress), 0, 0};
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, fileXferCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xferCtx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);

    // Range header for resume
    struct curl_slist *headerList = nullptr;
    if (resumeFrom > 0) {
        char range[64];
        std::snprintf(range, sizeof(range), "Range: bytes=%lu-",
                      static_cast<unsigned long>(resumeFrom));
        headerList = curl_slist_append(headerList, range);
    }
    for (const auto &h : headers) {
        headerList = curl_slist_append(headerList, h.c_str());
    }
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    char ua[128];
    std::snprintf(ua, sizeof(ua), "%s/%s", APP_NAME, VERSION_STR);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);

    TelemetryTimer timer;
    CURLcode res = curl_easy_perform(curl);

    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);

    if (headerList) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
        curl_slist_free_all(headerList);
    }

    bool ok = true;

    if (res != CURLE_OK) {
        error = std::string("Transport: ") + curl_easy_strerror(res);
        ok = false;
    } else if (resumeFrom > 0 && httpStatus == 200) {
        // Server ignored Range and sent full body — truncate and restart.
        std::fclose(f);
        f = std::fopen(destTmpPath.c_str(), "wb");
        if (!f) {
            error = "Failed to reopen output file for truncation";
            return false;
        }
        writeCtx.f = f;
        writeCtx.written = 0;

        // Re-configure without Range header
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
        headerList = nullptr;
        for (const auto &h : headers) {
            headerList = curl_slist_append(headerList, h.c_str());
        }
        if (headerList)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        if (headerList) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, nullptr);
            curl_slist_free_all(headerList);
            headerList = nullptr;
        }
        if (res != CURLE_OK) {
            error = std::string("Transport: ") + curl_easy_strerror(res);
            ok = false;
        }
    }

    // Check HTTP status — on error, delete any partial file to prevent
    // corruption from being resumed later (M3).
    if (ok && httpStatus != 200 && httpStatus != 206) {
        // M4: HTTP 416 (Range Not Satisfiable) with resumeFrom > 0
        // means the server has nothing more to send — the .part file
        // is already complete.  Leave it intact and report success.
        if (httpStatus == 416 && resumeFrom > 0) {
            if (f) { std::fflush(f); std::fclose(f); f = nullptr; }
            if (outBytes) *outBytes = resumeFrom;
            return true;
        }

        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", httpStatus);
        error = buf;
        ok = false;
        // Close and delete the partial file so it isn't resumed later.
        if (f) {
            std::fclose(f);
            f = nullptr;
        }
        ::unlink(destTmpPath.c_str());
    }

    // Flush and fsync before close
    if (f) {
        std::fflush(f);
        if (ok) {
            int fd = ::fileno(f);
            if (::fsync(fd) != 0 && errno == ENOSPC) {
                error = "Disk full (ENOSPC)";
                ok = false;
            }
        }
        std::fclose(f);
    }

    if (outBytes) *outBytes = writeCtx.written;

    recordNetworkRequest(timer, 1, httpStatus, res, writeCtx.written, 0,
                         res == CURLE_ABORTED_BY_CALLBACK, false);

    return ok;
}

} // namespace miyoofin
