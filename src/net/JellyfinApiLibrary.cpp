#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <cstdio>
#include <ctime>
#include <limits>

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
    return getViews(baseUrl, accessToken, userId, deviceId, views, error,
                    client, cancelled);
}

bool JellyfinApi::getViews(const std::string &baseUrl,
                           const std::string &accessToken,
                           const std::string &userId,
                           const std::string &deviceId,
                           std::vector<LibraryView> &views,
                           std::string &error,
                           HttpClient &client,
                           const std::atomic<bool> *cancelled)
{
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
    const std::string rawItems = jsonRawValue(response.body, "Items");
    if (rawItems.empty() || rawItems.front() != '[') {
        error = "Malformed library views response";
        return false;
    }
    auto itemStrs = jsonExtractArray(response.body, "Items");
    if (itemStrs.empty() && rawItems != "[]") {
        error = "Malformed library views response";
        return false;
    }
    for (const auto &s : itemStrs) {
        LibraryView v;
        v.id             = jsonStringField(s, "Id");
        v.name           = jsonStringField(s, "Name");
        v.collectionType = jsonStringField(s, "CollectionType");
        if (!v.id.empty()) views.push_back(std::move(v));
    }
    return true;
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
    HttpClient client;
    return getLibraryItemsPage(baseUrl, accessToken, userId, deviceId,
                               parentId, includeItemTypes, startIndex, limit,
                               page, error, client, cancelled);
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
                                      HttpClient &client,
                                      const std::atomic<bool> *cancelled)
{
    if (startIndex < 0 || limit <= 0) { error = "Invalid library page"; return false; }
    if (cancelled && cancelled->load()) { error = "Callback aborted"; return false; }
    client.setTimeoutSec(15);
    HttpResponse response;
    TelemetryRequestScope request(RequestKind::LibraryItemsPage);
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
    const std::string rawItems = jsonRawValue(response.body, "Items");
    if (rawItems.empty() || rawItems.front() != '[') {
        error = "Malformed library page response";
        return false;
    }
    const auto itemStrings = jsonExtractArray(response.body, "Items");
    if (itemStrings.empty() && rawItems != "[]") {
        error = "Malformed library page response";
        return false;
    }
    for (const auto &raw : itemStrings)
        page.items.push_back(jsonToMediaItem(raw));
    page.hasMore = page.startIndex + static_cast<int>(page.items.size()) < page.totalRecordCount;
    std::printf("[JellyfinApi] library_page_success start=%d count=%zu total=%d more=%d\n",
                page.startIndex, page.items.size(), page.totalRecordCount,
                page.hasMore ? 1 : 0);
    return true;
}

bool JellyfinApi::getChangedCatalogItems(
    const std::string &baseUrl, const std::string &accessToken,
    const std::string &userId, const std::string &deviceId,
    std::int64_t sinceMs, std::vector<MediaItem> &items,
    std::string &error, const std::atomic<bool> *cancelled)
{
    if (sinceMs <= 0) {
        error = "missing sync checkpoint";
        return false;
    }

    // MinDateLastSaved is second-granular.  Include the preceding second so
    // an item saved at the checkpoint boundary cannot be skipped.
    std::time_t seconds = static_cast<std::time_t>(sinceMs / 1000);
    if (seconds > 0) --seconds;
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &seconds);
#else
    gmtime_r(&seconds, &utc);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%S.0000000Z", &utc);

    HttpClient client;
    client.setTimeoutSec(15);
    const auto headers = buildAuthHeaders(accessToken, deviceId);
    constexpr int limit = 100;
    constexpr std::size_t maxChangedItems = 4096;
    int startIndex = 0;
    std::vector<MediaItem> changed;
    for (;;) {
        if (cancelled && cancelled->load()) {
            error = "Callback aborted";
            return false;
        }
        const std::string url = baseUrl + "/Users/" + userId
            + "/Items?Recursive=true&IncludeItemTypes=Movie,Series"
              "&SortBy=DateLastSaved&SortOrder=Ascending"
              "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,"
              "RunTimeTicks,SeriesName,SeriesId,SeasonId,ParentIndexNumber,"
              "IndexNumber,Etag&MinDateLastSaved=" + stamp
            + "&StartIndex=" + std::to_string(startIndex)
            + "&Limit=" + std::to_string(limit);
        HttpResponse response;
        TelemetryRequestScope request(RequestKind::ChangedCatalogItems);
        if (!client.perform("GET", url, headers, {}, response, error,
                            cancelled)) {
            if (error.empty()) error = "Could not reach server";
            return false;
        }
        if (!response.ok()) {
            error = "Changed catalog items failed (HTTP "
                + std::to_string(response.status) + ")";
            return false;
        }

        const std::string rawItems = jsonRawValue(response.body, "Items");
        if (rawItems.empty() || rawItems.front() != '[') {
            error = "Malformed changed catalog response";
            return false;
        }
        const auto itemStrings = jsonExtractArray(response.body, "Items");
        if (itemStrings.empty() && rawItems != "[]") {
            error = "Malformed changed catalog response";
            return false;
        }
        if (changed.size() + itemStrings.size() > maxChangedItems) {
            error = "Changed catalog result exceeds bounded limit";
            return false;
        }
        for (const auto &raw : itemStrings) {
            MediaItem item = jsonToMediaItem(raw);
            if (item.id.empty() || (item.type != "movie" && item.type != "show")) {
                error = "Malformed changed catalog item";
                return false;
            }
            changed.push_back(std::move(item));
        }
        if (itemStrings.size() < static_cast<std::size_t>(limit)) {
            items.insert(items.end(), changed.begin(), changed.end());
            return true;
        }
        if (startIndex > std::numeric_limits<int>::max() - limit) {
            error = "changed catalog pagination overflow";
            return false;
        }
        startIndex += limit;
    }
}

bool JellyfinApi::getItemsByIds(
    const std::string &baseUrl, const std::string &accessToken,
    const std::string &userId, const std::string &deviceId,
    const std::vector<std::string> &itemIds, std::vector<MediaItem> &items,
    std::string &error, const std::atomic<bool> *cancelled)
{
    constexpr std::size_t maxItemIds = 64;
    items.clear();
    if (itemIds.empty() || itemIds.size() > maxItemIds) {
        error = "item-by-ID query exceeds bounded limit";
        return false;
    }
    for (const auto &id : itemIds) {
        if (id.empty()) {
            error = "item-by-ID query contains an empty ID";
            return false;
        }
    }
    if (cancelled && cancelled->load()) {
        error = "Callback aborted";
        return false;
    }

    std::string joinedIds;
    for (const auto &id : itemIds) {
        if (!joinedIds.empty()) joinedIds += ',';
        joinedIds += id;
    }
    const std::string url = baseUrl + "/Users/" + userId + "/Items?Ids="
        + joinedIds
        + "&IncludeItemTypes=Movie,Series,Season,Episode"
          "&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,"
          "RunTimeTicks,SeriesName,SeriesId,SeasonId,ParentIndexNumber,"
          "IndexNumber,Etag";
    HttpClient client;
    client.setTimeoutSec(15);
    HttpResponse response;
    TelemetryRequestScope request(RequestKind::ItemsByIds);
    if (!client.perform("GET", url.c_str(), buildAuthHeaders(accessToken, deviceId),
                       {}, response, error, cancelled)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        error = "Items by ID failed (HTTP "
            + std::to_string(response.status) + ")";
        return false;
    }
    const std::string rawItems = jsonRawValue(response.body, "Items");
    if (rawItems.empty() || rawItems.front() != '[') {
        error = "Malformed items-by-ID response";
        return false;
    }
    const auto itemStrings = jsonExtractArray(response.body, "Items");
    if (itemStrings.empty() && rawItems != "[]") {
        error = "Malformed items-by-ID response";
        return false;
    }
    for (const auto &raw : itemStrings) {
        MediaItem item = jsonToMediaItem(raw);
        if (item.id.empty()
            || (item.type != "movie" && item.type != "show"
                && item.type != "season" && item.type != "episode")) {
            error = "Malformed items-by-ID item";
            items.clear();
            return false;
        }
        items.push_back(std::move(item));
    }
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
    return getResumeItems(baseUrl, accessToken, userId, deviceId, limit,
                          items, error, client, cancelled);
}

bool JellyfinApi::getResumeItems(const std::string &baseUrl,
                                 const std::string &accessToken,
                                 const std::string &userId,
                                 const std::string &deviceId,
                                 int limit,
                                 std::vector<MediaItem> &items,
                                 std::string &error,
                                 HttpClient &client,
                                 const std::atomic<bool> *cancelled)
{
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
    return getLatestItems(baseUrl, accessToken, userId, deviceId, limit,
                          items, error, client, cancelled);
}

bool JellyfinApi::getLatestItems(const std::string &baseUrl,
                                 const std::string &accessToken,
                                 const std::string &userId,
                                 const std::string &deviceId,
                                 int limit,
                                 std::vector<MediaItem> &items,
                                 std::string &error,
                                 HttpClient &client,
                                 const std::atomic<bool> *cancelled)
{
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
