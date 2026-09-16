#include "JellyfinApi.hpp"
#include "HttpClient.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <cstdio>
#include <ctime>
#include <limits>

namespace miyoofin {

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
