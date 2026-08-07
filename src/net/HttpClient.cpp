#include "HttpClient.hpp"
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

HttpClient::HttpClient()
{
    // Global init is handled once in main via curl_global_init
}

bool HttpClient::get(const std::string &url,
                     std::string &responseBody,
                     long &httpCode,
                     std::string &error)
{
    responseBody.clear();
    httpCode = 0;
    error.clear();

    CURL *curl = curl_easy_init();
    if (!curl) {
        error = "Failed to initialise libcurl easy handle";
        return false;
    }

    // Configure the request
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, m_timeoutSec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "MiyooFin/0.1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);   // no cert validation
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);   // for embedded simplicity
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 8192L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (httpCode != 200) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP %ld", httpCode);
        error = buf;
        return false;
    }

    if (responseBody.empty()) {
        error = "Empty response body";
        return false;
    }

    return true;
}

} // namespace miyoofin