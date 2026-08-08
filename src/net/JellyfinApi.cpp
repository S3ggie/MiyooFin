#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cstdint>

namespace miyoofin {

// -------------------------------------------------------------------
// Helper — decode \uXXXX (and UTF-16 surrogate pairs) to UTF-8.
// `pos` must point at the 'u' in the escape.  Returns the number of
// source characters consumed (1 for \uXXXX, 2 for a surrogate pair)
// and appends the UTF-8 encoding to `out`.
// -------------------------------------------------------------------
static int decodeUnicodeEscape(const std::string &s, size_t pos,
                               std::string &out)
{
    // pos points at 'u'
    if (pos + 4 >= s.size()) return 1; // malformed – skip
    auto hex = [&](unsigned idx) -> int {
        char c = s[pos + 1 + idx];
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int h0 = hex(0), h1 = hex(1), h2 = hex(2), h3 = hex(3);
    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return 1; // bad hex
    uint32_t cp = (uint32_t(h0) << 12) | (uint32_t(h1) << 8)
                | (uint32_t(h2) << 4)  |  uint32_t(h3);
    int consumed = 5; // \uXXXX → 5 chars: \, u, x, x, x

    // UTF-16 surrogate pair?
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        // look for \uXXXX (low surrogate) immediately after
        if (pos + 10 < s.size() && s[pos + 5] == '\\' && s[pos + 6] == 'u') {
            auto lhex = [&](unsigned idx) -> int {
                char c = s[pos + 7 + idx];
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int lh0 = lhex(0), lh1 = lhex(1), lh2 = lhex(2), lh3 = lhex(3);
            if (lh0 >= 0 && lh1 >= 0 && lh2 >= 0 && lh3 >= 0) {
                uint32_t lo = (uint32_t(lh0)<<12)|(uint32_t(lh1)<<8)
                            |(uint32_t(lh2)<<4)|uint32_t(lh3);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    consumed = 11; // \uHHHH\uLLLL
                }
            }
        }
        // if no valid low surrogate, emit the replacement char for the
        // lone high surrogate
        if (consumed == 5) cp = 0xFFFD;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        cp = 0xFFFD; // lone low surrogate
    }

    // Encode cp as UTF-8
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
    return consumed;
}

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
            else if (json[pos] == 'u' && pos + 4 < json.size())
                pos += decodeUnicodeEscape(json, pos, val) - 1;
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
            else if (json[afterChild] == 'u' && afterChild + 4 < json.size())
                afterChild += decodeUnicodeEscape(json, afterChild, val) - 1;
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

std::string JellyfinApi::jsonRawValue(const std::string &json,
                                      const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' '
           || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r'))
        pos++;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        pos++; std::string val;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) { pos++;
                switch (json[pos]) {
                    case 'n': val += '\n'; break;
                    case 'r': val += '\r'; break;
                    case 't': val += '\t'; break;
                    case '\\': val += '\\'; break;
                    case '"': val += '"'; break;
                    case 'u': pos += decodeUnicodeEscape(json, pos, val) - 1; break;
                    default: val += json[pos]; break;
                }
            } else { val += json[pos]; }
            pos++;
        }
        return val;
    } else if (json[pos] == '{' || json[pos] == '[') {
        char open = json[pos], close = (open == '{') ? '}' : ']';
        int depth = 1; size_t start = pos; pos++;
        while (pos < json.size() && depth > 0) {
            if (json[pos] == '"') { pos++;
                while (pos < json.size() && json[pos] != '"') {
                    if (json[pos] == '\\') pos++;
                    pos++; }
            } else if (json[pos] == open) depth++;
            else if (json[pos] == close) depth--;
            pos++;
        }
        return json.substr(start, pos - start);
    } else {
        size_t start = pos;
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}'
               && json[pos] != ']' && json[pos] != ' '
               && json[pos] != '\n' && json[pos] != '\r'
               && json[pos] != '\t') pos++;
        return json.substr(start, pos - start);
    }
}

std::string JellyfinApi::jsonStringField(const std::string &obj,
                                         const std::string &key)
{ return jsonRawValue(obj, key); }

int JellyfinApi::jsonIntField(const std::string &obj, const std::string &key)
{
    std::string v = jsonRawValue(obj, key);
    if (v.empty() || v == "null") return 0;
    try { return std::stoi(v); } catch (...) { return 0; }
}

float JellyfinApi::jsonFloatField(const std::string &obj,
                                  const std::string &key)
{
    std::string v = jsonRawValue(obj, key);
    if (v.empty() || v == "null") return 0.0f;
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        v = v.substr(1, v.size() - 2);
    try { return std::stof(v); } catch (...) { return 0.0f; }
}

bool JellyfinApi::jsonBoolField(const std::string &obj,
                                const std::string &key)
{ return jsonRawValue(obj, key) == "true"; }

std::vector<std::string> JellyfinApi::splitJsonArrayContent(
    const std::string &content)
{
    std::vector<std::string> result;
    size_t es = 0; int bd = 0; bool ins = false;
    for (size_t i = 0; i <= content.size(); i++) {
        if (i < content.size()) {
            char c = content[i];
            if (ins) {
                if (c=='\\'&&i+1<content.size()) i++;
                else if(c=='"') ins=false;
            } else {
                if (c=='"') ins=true;
                else if (c=='{'||c=='[') bd++;
                else if (c=='}'||c==']') bd--;
                else if (c==','&&bd==0) {
                    std::string e = content.substr(es, i-es);
                    size_t a=0;
                    while(a<e.size()&&(e[a]==' '||e[a]=='\n'||e[a]=='\r'||e[a]=='\t'))a++;
                    size_t b=e.size();
                    while(b>a&&(e[b-1]==' '||e[b-1]=='\n'||e[b-1]=='\r'||e[b-1]=='\t'))b--;
                    if(a<b) result.push_back(e.substr(a,b-a));
                    es=i+1;
                }
            }
        } else {
            std::string e = content.substr(es, i-es);
            size_t a=0;
            while(a<e.size()&&(e[a]==' '||e[a]=='\n'||e[a]=='\r'||e[a]=='\t'))a++;
            size_t b=e.size();
            while(b>a&&(e[b-1]==' '||e[b-1]=='\n'||e[b-1]=='\r'||e[b-1]=='\t'))b--;
            if(a<b) result.push_back(e.substr(a,b-a));
        }
    }
    return result;
}

std::vector<std::string> JellyfinApi::jsonExtractArray(
    const std::string &json, const std::string &key)
{
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return {};
    pos += search.size();
    while (pos < json.size() && (json[pos]==':'||json[pos]==' '
           ||json[pos]=='\t'||json[pos]=='\n'||json[pos]=='\r')) pos++;
    if (pos >= json.size() || json[pos] != '[') return {};
    pos++;
    size_t as = pos; int d = 1;
    while (pos < json.size() && d > 0) {
        if (json[pos] == '"') { pos++;
            while (pos < json.size() && json[pos] != '"') {
                if (json[pos] == '\\') pos++;
                pos++; }
        } else if (json[pos] == '[') d++;
        else if (json[pos] == ']') { d--; if(d==0) break; }
        pos++;
    }
    if (d != 0) return {};
    return splitJsonArrayContent(json.substr(as, pos - as));
}

MediaItem JellyfinApi::jsonToMediaItem(const std::string &obj)
{
    MediaItem item;
    item.id       = jsonStringField(obj, "Id");
    item.title    = jsonStringField(obj, "Name");
    item.overview = jsonStringField(obj, "Overview");
    item.year     = jsonIntField(obj, "ProductionYear");
    item.rating   = jsonFloatField(obj, "CommunityRating");
    item.type     = jsonStringField(obj, "Type");
    if (item.type == "Movie") item.type = "movie";
    else if (item.type == "Series") item.type = "show";
    else if (item.type == "Episode") item.type = "episode";
    else if (item.type == "Season") item.type = "season";

    item.indexNumber = jsonIntField(obj, "IndexNumber");

    // B5e2a: episode metadata
    item.parentIndexNumber = jsonIntField(obj, "ParentIndexNumber");
    {
        std::string rt = jsonRawValue(obj, "RunTimeTicks");
        if (!rt.empty() && rt != "null") {
            try { item.runTimeTicks = std::stoll(rt); } catch (...) { item.runTimeTicks = 0; }
        }
    }
    item.seriesName = jsonStringField(obj, "SeriesName");
    item.seriesId   = jsonStringField(obj, "SeriesId");
    item.seasonId   = jsonStringField(obj, "SeasonId");

    // Genres
    std::string gr = jsonRawValue(obj, "Genres");
    if (!gr.empty() && gr[0] == '[') {
        auto gs = jsonExtractArray(obj, "Genres");
        for (auto &g : gs) {
            if (g.size()>=2 && g.front()=='"' && g.back()=='"')
                g = g.substr(1, g.size()-2);
            if (!g.empty()) item.genres.push_back(g);
        }
        if (!item.genres.empty()) item.genre = item.genres[0];
    }

    // UserData
    std::string ur = jsonRawValue(obj, "UserData");
    if (!ur.empty() && ur[0] == '{') {
        item.played   = jsonBoolField(ur, "Played");
        item.progress = jsonFloatField(ur, "PlayedPercentage") / 100.0f;
        if (item.progress < 0.0f) item.progress = 0.0f;
        if (item.progress > 1.0f) item.progress = 1.0f;
    }

    // ImageTags
    std::string ir = jsonRawValue(obj, "ImageTags");
    if (!ir.empty() && ir[0] == '{') {
        size_t p = 1;
        while (p < ir.size()) {
            while (p < ir.size() && ir[p] != '"') p++;
            if (p >= ir.size()) break;
            p++;
            size_t ks = p;
            while (p < ir.size() && ir[p] != '"') p++;
            std::string ik = ir.substr(ks, p-ks); p++;
            while (p < ir.size() && ir[p] != ':') p++;
            if (p >= ir.size()) break;
            p++;
            while (p < ir.size() && ir[p] == ' ') p++;
            if (p < ir.size() && ir[p] == '"') { p++; size_t vs = p;
                while (p < ir.size() && ir[p] != '"') p++;
                item.imageTags[ik] = ir.substr(vs, p-vs); p++;
            }
            while (p < ir.size() && ir[p] != ',' && ir[p] != '}') p++;
            if (p < ir.size() && ir[p] == ',') p++;
        }
    }

    // Placeholder art colour
    Uint8 r = (Uint8)((item.title.size()*37+80)&0xFF);
    Uint8 g = (Uint8)((item.title.size()*53+160)&0xFF);
    Uint8 b = (Uint8)((item.title.size()*71+240)&0xFF);
    item.artR = 80+r%120; item.artG = 80+g%120; item.artB = 80+b%120;
    return item;
}

std::vector<TabData> JellyfinApi::buildTabs(
    const std::vector<LibraryView> & /*views*/,
    const std::vector<MediaItem> &continueWatching,
    const std::vector<MediaItem> &recentlyAdded,
    const std::vector<std::pair<std::string, std::vector<MediaItem>>> &moviesByView,
    const std::vector<std::pair<std::string, std::vector<MediaItem>>> &showsByView)
{
    std::vector<TabData> tabs;

    TabData home;
    home.name = "Home";
    if (!continueWatching.empty())
        home.rows.push_back({"Continue Watching", continueWatching});
    if (!recentlyAdded.empty())
        home.rows.push_back({"Recently Added", recentlyAdded});
    if (home.rows.empty()) home.rows.push_back({"", {}});
    tabs.push_back(std::move(home));

    TabData movies;
    movies.name = "Movies";
    for (const auto &pr : moviesByView)
        if (!pr.second.empty()) movies.rows.push_back({pr.first, pr.second});
    if (movies.rows.empty()) movies.rows.push_back({"No movies found", {}});
    tabs.push_back(std::move(movies));

    TabData shows;
    shows.name = "Shows";
    for (const auto &pr : showsByView)
        if (!pr.second.empty()) shows.rows.push_back({pr.first, pr.second});
    if (shows.rows.empty()) shows.rows.push_back({"No shows found", {}});
    tabs.push_back(std::move(shows));

    tabs.push_back({"Search",    {{"", {}}}});
    tabs.push_back({"Downloads", {{"", {}}}});
    return tabs;
}

// ===================================================================
// Checkpoint B4: Library fetching API methods
// ===================================================================

bool JellyfinApi::getViews(const std::string &baseUrl,
                           const std::string &accessToken,
                           const std::string &userId,
                           const std::string &deviceId,
                           std::vector<LibraryView> &views,
                           std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    std::string url = baseUrl + "/Users/" + userId + "/Views";
    HttpResponse response;
    if (!client.perform("GET", url, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Failed to fetch libraries (HTTP %ld)",
                      response.status);
        error = buf;
        return false;
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    for (const auto &s : itemStrs) {
        LibraryView v;
        v.id             = jsonStringField(s, "Id");
        v.name           = jsonStringField(s, "Name");
        v.collectionType = jsonStringField(s, "CollectionType");
        if (!v.id.empty()) views.push_back(std::move(v));
    }
    return true;
}

bool JellyfinApi::getLibraryItems(const std::string &baseUrl,
                                  const std::string &accessToken,
                                  const std::string &userId,
                                  const std::string &deviceId,
                                  const std::string &parentId,
                                  const std::string &includeItemTypes,
                                  int limit,
                                  std::vector<MediaItem> &items,
                                  std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(15);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    char urlBuf[512];
    std::snprintf(urlBuf, sizeof(urlBuf),
        "%s/Users/%s/Items?ParentId=%s&IncludeItemTypes=%s"
        "&SortBy=SortName&SortOrder=Ascending&Recursive=true"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags"
        "&Limit=%d",
        baseUrl.c_str(), userId.c_str(), parentId.c_str(),
        includeItemTypes.c_str(), limit);
    HttpResponse response;
    if (!client.perform("GET", urlBuf, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Failed to fetch items (HTTP %ld)",
                      response.status);
        error = buf;
        return false;
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    for (const auto &s : itemStrs)
        items.push_back(jsonToMediaItem(s));
    return true;
}

bool JellyfinApi::getResumeItems(const std::string &baseUrl,
                                 const std::string &accessToken,
                                 const std::string &userId,
                                 const std::string &deviceId,
                                 int limit,
                                 std::vector<MediaItem> &items,
                                 std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    char urlBuf[512];
    std::snprintf(urlBuf, sizeof(urlBuf),
        "%s/Users/%s/Items/Resume?Limit=%d&Recursive=true"
        "&IncludeItemTypes=Movie,Episode"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags",
        baseUrl.c_str(), userId.c_str(), limit);
    HttpResponse response;
    if (!client.perform("GET", urlBuf, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        if (response.status == 401) { error = "Unauthorized"; return false; }
        return true;  // treat other errors as empty (endpoint may not exist)
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    for (const auto &s : itemStrs)
        items.push_back(jsonToMediaItem(s));
    return true;
}

std::string JellyfinApi::buildLatestUrl(const std::string &baseUrl,
                                        const std::string &userId,
                                        int limit)
{
    char urlBuf[512];
    std::snprintf(urlBuf, sizeof(urlBuf),
        "%s/Users/%s/Items/Latest?Limit=%d"
        "&GroupItems=false"
        "&IncludeItemTypes=Movie,Series"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags",
        baseUrl.c_str(), userId.c_str(), limit);
    return std::string(urlBuf);
}

bool JellyfinApi::getLatestItems(const std::string &baseUrl,
                                 const std::string &accessToken,
                                 const std::string &userId,
                                 const std::string &deviceId,
                                 int limit,
                                 std::vector<MediaItem> &items,
                                 std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    std::string url = buildLatestUrl(baseUrl, userId, limit);
    HttpResponse response;
    if (!client.perform("GET", url.c_str(), headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        if (response.status == 401) { error = "Unauthorized"; return false; }
        return true;
    }
    // /Items/Latest may return a direct array or {Items:[...]}
    auto itemStrs = jsonExtractArray(response.body, "Items");
    if (itemStrs.empty()) {
        size_t bpos = response.body.find('[');
        if (bpos != std::string::npos) {
            size_t p = bpos + 1; int d = 1;
            while (p < response.body.size() && d > 0) {
                if (response.body[p] == '"') { p++;
                    while (p < response.body.size() && response.body[p]!='"') {
                        if (response.body[p] == '\\') p++;
                        p++; }
                } else if (response.body[p] == '[') d++;
                else if (response.body[p] == ']') d--;
                p++;
            }
            if (d == 0 && p > bpos + 2)
                itemStrs = splitJsonArrayContent(
                    response.body.substr(bpos + 1, p - bpos - 2));
        }
    }
    for (const auto &s : itemStrs)
        items.push_back(jsonToMediaItem(s));
    return true;
}

bool JellyfinApi::getSeasons(const std::string &baseUrl,
                             const std::string &accessToken,
                             const std::string &userId,
                             const std::string &deviceId,
                             const std::string &seriesId,
                             std::vector<MediaItem> &seasons,
                             std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    char urlBuf[512];
    std::snprintf(urlBuf, sizeof(urlBuf),
        "%s/Shows/%s/Seasons?UserId=%s"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags",
        baseUrl.c_str(), seriesId.c_str(), userId.c_str());
    HttpResponse response;
    if (!client.perform("GET", urlBuf, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Failed to fetch seasons (HTTP %ld)",
                      response.status);
        error = buf;
        return false;
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    for (const auto &s : itemStrs)
        seasons.push_back(jsonToMediaItem(s));
    return true;
}

bool JellyfinApi::getEpisodes(const std::string &baseUrl,
                              const std::string &accessToken,
                              const std::string &userId,
                              const std::string &deviceId,
                              const std::string &seriesId,
                              const std::string &seasonId,
                              std::vector<MediaItem> &episodes,
                              std::string &error)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    char urlBuf[768];
    std::snprintf(urlBuf, sizeof(urlBuf),
        "%s/Shows/%s/Episodes?UserId=%s"
        "&SeasonId=%s"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,"
        "RunTimeTicks,SeriesName,SeriesId,SeasonId",
        baseUrl.c_str(), seriesId.c_str(), userId.c_str(),
        seasonId.c_str());
    HttpResponse response;
    if (!client.perform("GET", urlBuf, headers, {}, response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Failed to fetch episodes (HTTP %ld)",
                      response.status);
        error = buf;
        return false;
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    for (const auto &s : itemStrs)
        episodes.push_back(jsonToMediaItem(s));
    return true;
}

} // namespace miyoofin