#include "HttpClient.hpp"
#include "ClockCheck.hpp"
#include "TlsConfig.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include "miyoofin/version.hpp"
#include <curl/curl.h>
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

    CURL *curl = curl_easy_init();
    if (!curl) {
        error = "Failed to initialise libcurl easy handle";
        return false;
    }

    BinaryWriteContext ctx;
    ctx.data = &response.data;
    ctx.maxSize = maxSize;
    ctx.exceeded = false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, binaryWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_connectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    if (!configureTls(curl, url, &error)) { curl_easy_cleanup(curl); return false; }
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (cancelled) { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelCallback); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); }

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
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

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

    CURL *curl = curl_easy_init();
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
    if (!configureTls(curl, url, &error)) { curl_easy_cleanup(curl); return false; }
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);
    if (cancelled) { curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, cancelCallback); curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancelled); curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L); }

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
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    recordNetworkRequest(timer, method == "POST" ? 2 : 1,
                         response.status, res, response.body.size(),
                         postBody.size(), false, false);

    return true;
}

} // namespace miyoofin
