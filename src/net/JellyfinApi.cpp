#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace miyoofin {

bool JellyfinApi::getSystemInfo(const std::string &baseUrl,
                                ServerInfo &info,
                                std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(5);

    std::string url = baseUrl + "/System/Info/Public";
    std::string body;
    long httpCode = 0;

    if (!client.get(url, body, httpCode, error)) {
        return false;
    }

    // Minimal JSON parsing — find fields by key.
    // We avoid adding a full JSON library for this small client.
    auto findString = [&](const char *key) -> std::string {
        std::string search = "\"" + std::string(key) + "\"";
        auto pos = body.find(search);
        if (pos == std::string::npos) return {};
        pos += search.size();
        // Skip whitespace between key and value
        while (pos < body.size() && (body[pos] == ':' || body[pos] == ' ' || body[pos] == '\t')) {
            pos++;
        }
        if (pos >= body.size() || body[pos] != '\"') return {};
        pos++; // skip opening quote
        std::string val;
        while (pos < body.size() && body[pos] != '\"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                pos++; // skip escape
            }
            val += body[pos];
            pos++;
        }
        return val;
    };

    info.serverName       = findString("ServerName");
    info.version          = findString("Version");
    info.operatingSystem  = findString("OperatingSystem");

    if (info.serverName.empty()) {
        error = "Could not parse server name from response";
        return false;
    }

    return true;
}

std::string JellyfinApi::normaliseUrl(const std::string &input)
{
    // Trim leading whitespace
    std::string url = input;
    size_t start = 0;
    while (start < url.size() && std::isspace(static_cast<unsigned char>(url[start]))) {
        start++;
    }
    if (start > 0) {
        url = url.substr(start);
    }

    // Trim trailing whitespace
    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) {
        url.pop_back();
    }

    // Prepend http:// if no scheme
    if (url.find("://") == std::string::npos) {
        url = "http://" + url;
    }

    // Strip trailing slash
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    return url;
}

} // namespace miyoofin