#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <cstdio>
#include <ctime>
#include <limits>

namespace miyoofin {

namespace {
MediaItem jsonToChangedHierarchyItem(const std::string &obj)
{
    MediaItem item;
    item.id = JellyfinApi::jsonStringField(obj, "Id");
    item.type = JellyfinApi::jsonStringField(obj, "Type");
    if (item.type == "Movie") item.type = "movie";
    else if (item.type == "Series") item.type = "show";
    else if (item.type == "Episode") item.type = "episode";
    else if (item.type == "Season") item.type = "season";
    item.seriesId = JellyfinApi::jsonStringField(obj, "SeriesId");
    return item;
}
}

bool JellyfinApi::getChangedHierarchyItems(const std::string &baseUrl,const std::string &accessToken,const std::string &userId,const std::string &deviceId,std::int64_t sinceMs,std::vector<MediaItem>&items,std::string&error)
{
    if(sinceMs<=0){error="missing sync checkpoint";return false;}
    std::time_t seconds=(std::time_t)(sinceMs/1000); std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc,&seconds);
#else
    gmtime_r(&seconds,&utc);
#endif
    char stamp[32];std::strftime(stamp,sizeof(stamp),"%Y-%m-%dT%H:%M:%S.0000000Z",&utc);
    HttpClient client;client.setTimeoutSec(15);auto headers=buildAuthHeaders(accessToken,deviceId);int start=0;const int limit=100;
    for(;;){std::string url=baseUrl+"/Users/"+userId+"/Items?Recursive=true&IncludeItemTypes=Series,Season,Episode&SortBy=DateLastSaved&SortOrder=Ascending&Fields=SeriesId&MinDateLastSaved="+stamp+"&StartIndex="+std::to_string(start)+"&Limit="+std::to_string(limit);HttpResponse response;TelemetryRequestScope request(RequestKind::ChangedHierarchy);if(!client.perform("GET",url,headers,{},response,error)){if(error.empty())error="Could not reach server";return false;}if(!response.ok()){error="Changed items failed (HTTP "+std::to_string(response.status)+")";return false;}auto raw=jsonExtractArray(response.body,"Items");for(const auto&s:raw)items.push_back(jsonToChangedHierarchyItem(s));if(raw.size()<(size_t)limit)return true;if(start>std::numeric_limits<int>::max()-limit){error="changed item pagination overflow";return false;}start+=limit;}
}

bool JellyfinApi::getSeasons(const std::string &baseUrl,
                             const std::string &accessToken,
                             const std::string &userId,
                             const std::string &deviceId,
                             const std::string &seriesId,
                             std::vector<MediaItem> &seasons,
                             std::string &error, const std::atomic<bool> *cancelled)
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
    TelemetryRequestScope request(RequestKind::Seasons);
    if (!client.perform("GET", urlBuf, headers, {}, response, error, cancelled)) {
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
                              std::string &error, const std::atomic<bool> *cancelled)
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
    TelemetryRequestScope request(RequestKind::Episodes);
    if (!client.perform("GET", urlBuf, headers, {}, response, error, cancelled)) {
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
