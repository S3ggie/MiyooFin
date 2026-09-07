#include "JellyfinApi.hpp"
#include "HttpClient.hpp"

namespace miyoofin {

namespace {
PlaybackSyncStatus playbackStatus(bool transport, long status)
{
    if (!transport || status == 0 || status >= 500) return PlaybackSyncStatus::Transient;
    if (status == 401 || status == 403) return PlaybackSyncStatus::Unauthorized;
    if (status == 404) return PlaybackSyncStatus::Missing;
    return status >= 200 && status < 300 ? PlaybackSyncStatus::Success : PlaybackSyncStatus::Transient;
}
}

PlaybackSyncStatus JellyfinApi::getPlaybackPositionTicks(const std::string &baseUrl,
                                                         const std::string &accessToken,
                                                         const std::string &userId,
                                                         const std::string &deviceId,
                                                         const std::string &itemId,
                                                         std::int64_t &ticks,
                                                         std::string &error)
{
    ticks = 0;
    HttpClient client; HttpResponse response;
    const bool transport = client.perform("GET", baseUrl + "/Users/" + userId + "/Items/" + itemId,
        buildAuthHeaders(accessToken, deviceId), {}, response, error);
    PlaybackSyncStatus status = playbackStatus(transport, response.status);
    if (status != PlaybackSyncStatus::Success) return status;
    std::string userData = jsonRawValue(response.body, "UserData");
    std::string value = jsonRawValue(userData, "PlaybackPositionTicks");
    if (!value.empty() && value != "null") {
        try { ticks = std::stoll(value); } catch (...) { error = "invalid PlaybackPositionTicks"; return PlaybackSyncStatus::Transient; }
        if (ticks < 0) { error = "invalid PlaybackPositionTicks"; return PlaybackSyncStatus::Transient; }
    }
    return status;
}

PlaybackSyncStatus JellyfinApi::reportPlaybackStopped(const std::string &baseUrl,
                                                      const std::string &accessToken,
                                                      const std::string &deviceId,
                                                      const std::string &itemId,
                                                      std::int64_t ticks,
                                                      std::string &error)
{
    HttpClient client; HttpResponse response;
    const std::string body = "{\"ItemId\":\"" + jsonEscape(itemId) + "\",\"PositionTicks\":" +
        std::to_string(ticks < 0 ? 0 : ticks) + ",\"Failed\":false}";
    const bool transport = client.post(baseUrl + "/Sessions/Playing/Stopped",
        buildAuthHeaders(accessToken, deviceId), body, response, error);
    return playbackStatus(transport, response.status);
}


} // namespace miyoofin
