#include "JellyfinApi.hpp"
#include <curl/curl.h>
#include "HttpClient.hpp"
#include "../download/HlsPlaylist.hpp"
#include "../download/HlsProfile.hpp"
#include "miyoofin/version.hpp"
#include <cstdio>
#include <cctype>
#include <cstdint>
#include <limits>
#include <ctime>
#include <cstdlib>

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
    for(;;){std::string url=baseUrl+"/Users/"+userId+"/Items?Recursive=true&IncludeItemTypes=Series,Season,Episode&SortBy=DateLastSaved&SortOrder=Ascending&Fields=Overview,Genres,CommunityRating,UserData,ImageTags,RunTimeTicks,SeriesName,SeriesId,SeasonId&MinDateLastSaved="+stamp+"&StartIndex="+std::to_string(start)+"&Limit="+std::to_string(limit);HttpResponse response;if(!client.perform("GET",url,headers,{},response,error)){if(error.empty())error="Could not reach server";return false;}if(!response.ok()){error="Changed items failed (HTTP "+std::to_string(response.status)+")";return false;}auto raw=jsonExtractArray(response.body,"Items");for(const auto&s:raw)items.push_back(jsonToMediaItem(s));if(raw.size()<(size_t)limit)return true;if(start>std::numeric_limits<int>::max()-limit){error="changed item pagination overflow";return false;}start+=limit;}
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

bool JellyfinApi::getDownloadMediaSources(const std::string &baseUrl,
                                          const std::string &accessToken,
                                          const std::string &userId,
                                          const std::string &deviceId,
                                          const std::string &itemId,
                                          std::vector<DownloadMediaSource> &sources,
                                          std::string &error)
{
    if (itemId.empty()) { error = "Missing item id"; return false; }
    HttpClient client;
    client.setTimeoutSec(15);
    HttpResponse response;
    const std::string url = baseUrl + "/Items/" + itemId +
        "/PlaybackInfo?UserId=" + userId + "&StartTimeTicks=0";
    // Jellyfin expects a JSON PlaybackInfo payload with its content type; an
    // empty POST body is rejected as 415 by stricter server versions.
    auto headers = buildAuthHeaders(accessToken, deviceId);
    headers.push_back("Content-Type: application/json");
    if (!client.perform("POST", url, headers, "{\"UserId\":\"" + userId + "\",\"StartTimeTicks\":0}", response, error)) {
        if (error.empty()) error = "Could not reach server";
        return false;
    }
    if (!response.ok()) {
        error = response.status == 401 || response.status == 403 ? "Unauthorized" :
            "PlaybackInfo failed (HTTP " + std::to_string(response.status) + ")";
        return false;
    }
    for (const auto &obj : jsonExtractArray(response.body, "MediaSources")) {
        DownloadMediaSource s;
        s.id = jsonStringField(obj, "Id");
        s.container = jsonStringField(obj, "Container");
        s.etag = jsonStringField(obj, "ETag");
        s.size = static_cast<std::uint64_t>(std::max(0, jsonIntField(obj, "Size")));
        const std::string rawSize = jsonRawValue(obj, "Size");
        if (!rawSize.empty()) { char *end = nullptr; unsigned long long v = std::strtoull(rawSize.c_str(), &end, 10); if (end && !*end) s.size = v; }
        const std::string rawRuntime = jsonRawValue(obj, "RunTimeTicks");
        if (!rawRuntime.empty()) { char *end = nullptr; long long v = std::strtoll(rawRuntime.c_str(), &end, 10); if (end && !*end && v >= 0) s.runtimeTicks = v; }
        const std::string rawBitrate = jsonRawValue(obj, "Bitrate");
        if (!rawBitrate.empty()) { char *end = nullptr; unsigned long long v = std::strtoull(rawBitrate.c_str(), &end, 10); if (end && !*end) s.bitrate = v; }
        s.videoCodec = jsonStringField(obj, "VideoCodec"); s.audioCodec = jsonStringField(obj, "AudioCodec");
        s.width = jsonIntField(obj, "Width"); s.height = jsonIntField(obj, "Height");
        s.supportsDirectPlay = jsonBoolField(obj, "SupportsDirectPlay");
        s.supportsDirectStream = jsonBoolField(obj, "SupportsDirectStream");
        s.supportsTranscoding = jsonBoolField(obj, "SupportsTranscoding");
        for (const auto &stream : jsonExtractArray(obj, "MediaStreams"))
            if (jsonStringField(stream, "Type") == "Subtitle" && jsonBoolField(stream, "IsExternal")) s.hasExternalSubtitles = true;
        if (!s.id.empty()) sources.push_back(std::move(s));
    }
    if (sources.empty()) { error = "No media sources"; return false; }
    return true;
}

std::string JellyfinApi::buildHlsMasterUrl(const std::string&b,const std::string&i,const std::string&m,const std::string&p){return b+"/Videos/"+i+"/master.m3u8?Static=false&VideoCodec=h264&AudioCodec=aac&MaxWidth="+std::to_string(HLS_MAX_WIDTH)+"&MaxHeight="+std::to_string(HLS_MAX_HEIGHT)+"&MaxFramerate="+std::to_string(HLS_MAX_FRAMERATE)+"&MaxVideoBitDepth=8&VideoBitRate="+std::to_string(HLS_VIDEO_BITRATE)+"&AudioBitRate="+std::to_string(HLS_AUDIO_BITRATE)+"&AudioChannels=2&MaxAudioChannels=2&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false&EnableAutoStreamCopy=false&Context=Streaming&SubtitleStreamIndex=-1"+(m.empty()?"":"&MediaSourceId="+m)+(p.empty()?"":"&PlaySessionId="+p);}
JellyfinApi::HlsFailure JellyfinApi::classifyHlsFailure(long status, int transportCode) {
    if (status == 401 || status == 403) return HlsFailure::Unauthorized;
    if (status >= 400) return HlsFailure::Http;
    if (transportCode == CURLE_OPERATION_TIMEDOUT) return HlsFailure::Timeout;
    return transportCode ? HlsFailure::Network : HlsFailure::Playlist;
}
bool JellyfinApi::getHlsSegmentUrls(const std::string&b,const std::string&t,const std::string&d,const std::string&i,const std::string&m,std::vector<std::string>&out,std::string&e,HlsFailure *failure){
    if(failure)*failure=HlsFailure::None;
    HttpClient c; c.setConnectTimeoutSec(10); c.setTimeoutSec(60); HttpResponse r;
    auto fail=[&](const char *stage){ HlsFailure f=classifyHlsFailure(r.status,r.transportCode); if(failure)*failure=f; if(r.status) e=std::string(stage)+" HTTP "+std::to_string(r.status); else if(f==HlsFailure::Timeout) e="Preparing transcode timed out"; else if(e.empty()) e=std::string(stage)+" request failed"; return false; };
    std::string u=buildHlsMasterUrl(b,i,m); std::printf("[Download] HLS master request...\n");
    if(!c.perform("GET",u,buildAuthHeaders(t,d),{},r,e)||!r.ok()){std::printf("[Download] HLS master HTTP %ld\n",r.status);return fail("HLS master");}
    std::printf("[Download] HLS master HTTP %ld\n",r.status); auto v=HlsPlaylist::variants(r.body,u);
    if(!v.empty()){u=v.front();std::printf("[Download] HLS variant request...\n");if(!c.perform("GET",u,buildAuthHeaders(t,d),{},r,e)||!r.ok()){std::printf("[Download] HLS variant HTTP %ld\n",r.status);return fail("HLS variant");}std::printf("[Download] HLS variant HTTP %ld\n",r.status);}
    out=HlsPlaylist::segments(r.body,u);if(out.empty()){if(failure)*failure=HlsFailure::Playlist;e="HLS playlist has no segments";return false;}std::printf("[Download] HLS segments=%zu\n",out.size());return true;
}
} // namespace miyoofin
