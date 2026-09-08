#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cctype>

namespace miyoofin {

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

    TelemetryRequestScope request(RequestKind::SystemInfo);
    if (!client.get(url, body, httpCode, error)) {
        return false;
    }

    if (!parseSystemInfoResponse(body, info)) {
        error = "Could not parse server name from response";
        return false;
    }

    return true;
}

bool JellyfinApi::parseSystemInfoResponse(const std::string &body,
                                          ServerInfo &info)
{
    info = {};
    info.serverId         = extractString(body, "Id");
    info.serverName       = extractString(body, "ServerName");
    info.version          = extractString(body, "Version");
    info.operatingSystem  = extractString(body, "OperatingSystem");
    return !info.serverName.empty();
}

bool JellyfinApi::serverIdsMatch(const std::string &expectedServerId,
                                 const std::string &returnedServerId)
{
    return !expectedServerId.empty() && expectedServerId == returnedServerId;
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
    TelemetryRequestScope request(RequestKind::Authentication);
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
    return validateTokenStatus(baseUrl, accessToken, userId, deviceId, error) == TokenValidation::Valid;
}

TokenValidation JellyfinApi::validateTokenStatus(const std::string &baseUrl,
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
    TelemetryRequestScope request(RequestKind::TokenValidation);
    if (!client.perform("GET", url, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return TokenValidation::Unavailable;
    }

    if (response.ok()) {
        return TokenValidation::Valid;
    }

    if (response.status == 401) {
        error = "Session expired. Please log in again.";
    } else {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Token validation failed (HTTP %ld)", response.status);
        error = buf;
    }
    return response.status == 401 || response.status == 403
        ? TokenValidation::Unauthorized : TokenValidation::Unavailable;
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

// ===================================================================
// Checkpoint B4: auth helpers + JSON parsing
// ===================================================================

std::vector<std::string> JellyfinApi::buildAuthHeaders(
    const std::string &accessToken, const std::string &deviceId)
{
    std::vector<std::string> headers;
    char authLine[512];
    std::snprintf(authLine, sizeof(authLine), "X-Emby-Token: %s",
                  accessToken.c_str());
    headers.push_back(authLine);
    char identityHeader[512];
    std::snprintf(identityHeader, sizeof(identityHeader),
        "X-Emby-Authorization: MediaBrowser "
        "Client=\"%s\", Device=\"%s\", DeviceId=\"%s\", Version=\"%s\"",
        APP_NAME, DEVICE_NAME, deviceId.c_str(), VERSION_STR);
    headers.push_back(identityHeader);
    return headers;
}


} // namespace miyoofin
