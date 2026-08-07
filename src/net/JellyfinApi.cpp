#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>

namespace miyoofin {

// -------------------------------------------------------------------
// Helper — extract a top-level JSON string value.
// -------------------------------------------------------------------
std::string JellyfinApi::extractString(const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != '\"') return {};
    pos++;
    std::string val;
    while (pos < json.size() && json[pos] != '\"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) {
            pos++;
            if (json[pos] == 'n') val += '\n';
            else if (json[pos] == 'r') val += '\r';
            else if (json[pos] == 't') val += '\t';
            else if (json[pos] == '\\') val += '\\';
            else if (json[pos] == '\"') val += '\"';
            else val += json[pos];
        } else {
            val += json[pos];
        }
        pos++;
    }
    return val;
}

// -------------------------------------------------------------------
// Helper — extract a nested JSON string value.
// -------------------------------------------------------------------
std::string JellyfinApi::extractNestedString(const std::string &json,
                                        const std::string &parent,
                                        const std::string &child)
{
    std::string parentSearch = "\"" + parent + "\"";
    auto parentPos = json.find(parentSearch);
    if (parentPos == std::string::npos) return {};

    auto afterParent = parentPos + parentSearch.size();
    auto childPos = json.find("\"" + child + "\"", afterParent);
    if (childPos == std::string::npos) return {};

    auto afterChild = childPos + child.size() + 2;
    while (afterChild < json.size() &&
           (json[afterChild] == ':' || json[afterChild] == ' ' || json[afterChild] == '\t'))
        afterChild++;
    if (afterChild >= json.size() || json[afterChild] != '\"') return {};
    afterChild++;
    std::string val;
    while (afterChild < json.size() && json[afterChild] != '\"') {
        if (json[afterChild] == '\\' && afterChild + 1 < json.size()) {
            afterChild++;
            if (json[afterChild] == 'n') val += '\n';
            else if (json[afterChild] == 'r') val += '\r';
            else if (json[afterChild] == 't') val += '\t';
            else if (json[afterChild] == '\\') val += '\\';
            else if (json[afterChild] == '\"') val += '\"';
            else val += json[afterChild];
        } else {
            val += json[afterChild];
        }
        afterChild++;
    }
    return val;
}
// -------------------------------------------------------------------
// Helper — JSON escape for string values.
// -------------------------------------------------------------------
std::string JellyfinApi::jsonEscape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '\"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:   out += c;      break;
        }
    }
    return out;
}

// -------------------------------------------------------------------
// Classify an HTTP response from AuthenticateByName.
// -------------------------------------------------------------------
AuthError JellyfinApi::classifyAuthError(long httpStatus, const std::string &body,
                                    std::string &message)
{
    std::string serverMsg = extractNestedString(body, "Error", "Message");
    if (!serverMsg.empty()) message = serverMsg;

    switch (httpStatus) {
    case 200: return AuthError::None;
    case 400:
        if (message.empty()) message = "Username and password required.";
        return AuthError::BadRequest;
    case 401:
        if (message.empty()) message = "Invalid username or password.";
        return AuthError::InvalidCredentials;
    case 403:
        if (message.empty()) message = "Account disabled or not authorised.";
        return AuthError::Unauthorized;
    default:
        if (httpStatus >= 500 && httpStatus < 600) {
            if (message.empty()) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Server error (HTTP %ld)", httpStatus);
                message = buf;
            }
            return AuthError::ServerError;
        }
        // Other HTTP errors
        if (message.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "HTTP %ld", httpStatus);
            message = buf;
        }
        return AuthError::ServerError;
    }
}
// ===================================================================
// Public API
// ===================================================================

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

    info.serverName       = extractString(body, "ServerName");
    info.version          = extractString(body, "Version");
    info.operatingSystem  = extractString(body, "OperatingSystem");

    if (info.serverName.empty()) {
        error = "Could not parse server name from response";
        return false;
    }

    return true;
}

bool JellyfinApi::authenticateByName(const std::string &baseUrl,
                                     const std::string &username,
                                     const std::string &password,
                                     const std::string &deviceId,
                                     AuthResult &result,
                                     AuthError &errCode,
                                     std::string &error)
{
    result = {};
    errCode = AuthError::None;
    error.clear();

    // Build X-Emby-Authorization header
    char authHeader[512];
    std::snprintf(authHeader, sizeof(authHeader),
        "X-Emby-Authorization: MediaBrowser "
        "Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
        APP_NAME, DEVICE_NAME, deviceId.c_str(), VERSION_STR);

    // Build JSON body — password is never printed anywhere.
    std::string postBody = "{\"Username\":\""
                         + jsonEscape(username)
                         + "\",\"Pw\":\""
                         + jsonEscape(password)
                         + "\"}";

    HttpClient client;
    client.setTimeoutSec(10);

    std::vector<std::string> headers = {
        authHeader,
        "Content-Type: application/json"
    };

    HttpResponse response;
    if (!client.post(baseUrl + "/Users/AuthenticateByName", headers, postBody,
                     response, error)) {
        // Transport failure (server unreachable, timeout, DNS, etc.)
        errCode = AuthError::Network;
        if (error.empty()) error = "Could not reach server";
        return false;
    }

    if (response.ok()) {
        result.accessToken = extractString(response.body, "AccessToken");
        result.userId      = extractNestedString(response.body, "User", "Id");
        result.userName    = extractNestedString(response.body, "User", "Name");
        result.serverId    = extractString(response.body, "ServerId");

        if (result.accessToken.empty() || result.userId.empty()) {
            errCode = AuthError::ParseError;
            error = "Unexpected authentication response";
            return false;
        }

        return true;
    }

    errCode = classifyAuthError(response.status, response.body, error);
    return false;
}

bool JellyfinApi::validateToken(const std::string &baseUrl,
                                const std::string &accessToken,
                                const std::string &userId,
                                const std::string &deviceId,
                                std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(5);

    std::vector<std::string> headers;
    char authLine[512];
    std::snprintf(authLine, sizeof(authLine), "X-Emby-Token: %s", accessToken.c_str());
    headers.push_back(authLine);

    char identityHeader[512];
    std::snprintf(identityHeader, sizeof(identityHeader),
        "X-Emby-Authorization: MediaBrowser "
        "Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
        APP_NAME, DEVICE_NAME, deviceId.c_str(), VERSION_STR);
    headers.push_back(identityHeader);

    std::string url = baseUrl + "/Users/" + userId;

    HttpResponse response;
    if (!client.perform("GET", url, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }

    if (response.ok()) {
        return true;  // token is valid
    }

    if (response.status == 401) {
        error = "Session expired. Please log in again.";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Token validation failed (HTTP %ld)", response.status);
        error = buf;
    }
    return false;
}

std::string JellyfinApi::normaliseUrl(const std::string &input)
{
    std::string url = input;
    size_t start = 0;
    while (start < url.size() && std::isspace(static_cast<unsigned char>(url[start]))) {
        start++;
    }
    if (start > 0) url = url.substr(start);

    while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back()))) {
        url.pop_back();
    }

    if (url.find("://") == std::string::npos) {
        url = "http://" + url;
    }

    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    return url;
}

} // namespace miyoofin