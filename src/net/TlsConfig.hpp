#pragma once

#include <curl/curl.h>
#include <unistd.h>
#include <string>

namespace miyoofin {

// launch.sh changes into the installed application directory, where the
// packaged curl/Mozilla bundle is staged as cacert.pem.
inline bool configureTls(CURL *curl, const std::string &url, std::string *error = nullptr)
{
    if (url.compare(0, 8, "https://") != 0)
        return true; // Plain LAN HTTP neither needs nor consults CA data.
    static const char kCaBundle[] = "cacert.pem";
    if (access(kCaBundle, R_OK) != 0) {
        if (error) *error = "HTTPS requires packaged cacert.pem";
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, kCaBundle);
    return true;
}

} // namespace miyoofin
