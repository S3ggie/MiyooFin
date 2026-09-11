#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
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
                           std::string &error,
                           const std::atomic<bool> *cancelled)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    std::string url = baseUrl + "/Users/" + userId + "/Views";
    HttpResponse response;
    TelemetryRequestScope request(RequestKind::Views);
    if (!client.perform("GET", url, headers, {}, response, error, cancelled)) {
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
                                  std::string &error,
                                  const std::atomic<bool> *cancelled)
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
        if (cancelled && cancelled->load()) {
            error = "Callback aborted";
            return false;
        }
        std::string url = buildLibraryItemsUrl(baseUrl, userId, parentId,
                                               includeItemTypes, startIndex, limit);
        HttpResponse response;
        TelemetryRequestScope request(RequestKind::LibraryItems);
        if (!client.perform("GET", url.c_str(), headers, {}, response, error, cancelled)) {
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

bool JellyfinApi::getLibraryItemsPage(const std::string &baseUrl,
                                      const std::string &accessToken,
                                      const std::string &userId,
                                      const std::string &deviceId,
                                      const std::string &parentId,
                                      const std::string &includeItemTypes,
                                      int startIndex, int limit,
                                      LibraryItemsPage &page,
                                      std::string &error,
                                      const std::atomic<bool> *cancelled)
{
    if (startIndex < 0 || limit <= 0) { error = "Invalid library page"; return false; }
    if (cancelled && cancelled->load()) { error = "Callback aborted"; return false; }
    HttpClient client;
    client.setTimeoutSec(15);
    HttpResponse response;
    TelemetryRequestScope request(RequestKind::LibraryItems);
    if (!client.perform("GET", buildLibraryItemsUrl(baseUrl, userId, parentId,
                                                     includeItemTypes, startIndex, limit).c_str(),
                       buildAuthHeaders(accessToken, deviceId), {}, response,
                       error, cancelled)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) { error = "Failed to fetch library page"; return false; }
    page = {};
    page.startIndex = jsonIntField(response.body, "StartIndex");
    if (page.startIndex == 0 && startIndex != 0) page.startIndex = startIndex;
    page.totalRecordCount = jsonIntField(response.body, "TotalRecordCount");
    for (const auto &raw : jsonExtractArray(response.body, "Items"))
        page.items.push_back(jsonToMediaItem(raw));
    page.hasMore = page.startIndex + static_cast<int>(page.items.size()) < page.totalRecordCount;
    std::printf("[JellyfinApi] library_page_success start=%d count=%zu total=%d more=%d\n",
                page.startIndex, page.items.size(), page.totalRecordCount,
                page.hasMore ? 1 : 0);
    return true;
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
        "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,RunTimeTicks,SeriesName,SeriesId,SeasonId,ParentIndexNumber,IndexNumber,Etag"
        "&StartIndex=" + std::to_string(startIndex) +
        "&Limit=" + std::to_string(limit);
}

bool JellyfinApi::getResumeItems(const std::string &baseUrl,
                                 const std::string &accessToken,
                                 const std::string &userId,
                                 const std::string &deviceId,
                                 int limit,
                                 std::vector<MediaItem> &items,
                                 std::string &error,
                                 const std::atomic<bool> *cancelled)
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
    TelemetryRequestScope request(RequestKind::ResumeItems);
    if (!client.perform("GET", urlBuf, headers, {}, response, error, cancelled)) {
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
                                 std::string &error,
                                 const std::atomic<bool> *cancelled)
{
    HttpClient client;
    client.setTimeoutSec(10);
    auto headers = buildAuthHeaders(accessToken, deviceId);
    std::string url = buildLatestUrl(baseUrl, userId, limit);
    HttpResponse response;
    TelemetryRequestScope request(RequestKind::LatestItems);
    if (!client.perform("GET", url.c_str(), headers, {}, response, error, cancelled)) {
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
