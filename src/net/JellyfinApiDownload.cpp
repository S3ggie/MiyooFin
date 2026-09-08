#include "JellyfinApi.hpp"
#include <curl/curl.h>
#include "HttpClient.hpp"
#include "../download/HlsPlaylist.hpp"
#include "../download/HlsProfile.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <cstdio>

namespace miyoofin {

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
    TelemetryRequestScope request(RequestKind::DownloadPlaybackInfo);
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
    { TelemetryRequestScope request(RequestKind::HlsMaster); if(!c.perform("GET",u,buildAuthHeaders(t,d),{},r,e)||!r.ok()){std::printf("[Download] HLS master HTTP %ld\n",r.status);return fail("HLS master");} }
    std::printf("[Download] HLS master HTTP %ld\n",r.status); auto v=HlsPlaylist::variants(r.body,u);
    if(!v.empty()){u=v.front();std::printf("[Download] HLS variant request...\n");{ TelemetryRequestScope request(RequestKind::HlsVariant); if(!c.perform("GET",u,buildAuthHeaders(t,d),{},r,e)||!r.ok()){std::printf("[Download] HLS variant HTTP %ld\n",r.status);return fail("HLS variant");} }std::printf("[Download] HLS variant HTTP %ld\n",r.status);}
    out=HlsPlaylist::segments(r.body,u);if(out.empty()){if(failure)*failure=HlsFailure::Playlist;e="HLS playlist has no segments";return false;}std::printf("[Download] HLS segments=%zu\n",out.size());return true;
}

} // namespace miyoofin
