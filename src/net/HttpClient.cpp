#include "HttpClient.hpp"
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
                           size_t maxSize)
{
    response.status = 0;
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);

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

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
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

    return true;
}

bool HttpClient::perform(const std::string &method,
                         const std::string &url,
                         const std::vector<std::string> &headers,
                         const std::string &postBody,
                         HttpResponse &response,
                         std::string &error)
{
    response.status = 0;
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
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);   // no cert validation
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);   // for embedded simplicity
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);

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

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    return true;
}

} // namespace miyoofin