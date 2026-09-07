#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include <cstdio>

namespace miyoofin {

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

    tabs.push_back({"Downloads", {{"", {}}}});
    tabs.push_back({"Settings", {{"", {}}}});
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
    if (limit <= 0) {
        error = "Library item page size must be positive";
        return false;
    }

    HttpClient client;
    client.setTimeoutSec(15);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    int startIndex = 0;

    while (true) {
        std::string url = buildLibraryItemsUrl(baseUrl, userId, parentId,
                                               includeItemTypes, startIndex, limit);
        HttpResponse response;
        if (!client.perform("GET", url.c_str(), headers, {}, response, error)) {
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

        if (itemStrs.size() < static_cast<size_t>(limit))
            return true;

        if (startIndex > std::numeric_limits<int>::max() - limit) {
            error = "Library item pagination offset overflow";
            return false;
        }
        startIndex += limit;
    }
}


std::string JellyfinApi::buildLibraryItemsUrl(const std::string &baseUrl,
                                              const std::string &userId,
                                              const std::string &parentId,
                                              const std::string &includeItemTypes,
                                              int startIndex,
                                              int limit)
{
    return baseUrl + "/Users/" + userId + "/Items?ParentId=" + parentId +
        "&IncludeItemTypes=" + includeItemTypes +
        "&SortBy=SortName&SortOrder=Ascending&Recursive=true"
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags"
        "&StartIndex=" + std::to_string(startIndex) +
        "&Limit=" + std::to_string(limit);
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
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,"
        "RunTimeTicks,SeriesName,SeriesId,SeasonId,ParentIndexNumber",
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


} // namespace miyoofin
