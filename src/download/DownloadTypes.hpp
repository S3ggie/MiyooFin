#ifndef MIYOOFIN_DOWNLOAD_TYPES_HPP
#define MIYOOFIN_DOWNLOAD_TYPES_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <limits>
#include <algorithm>
#include <cstdio>
#include "HlsProfile.hpp"

namespace miyoofin {
constexpr std::uint64_t DOWNLOAD_CHUNK_SIZE = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t DOWNLOAD_SAFETY_RESERVE = 256ULL * 1024ULL * 1024ULL;

enum class DownloadState { Queued, Planning, Downloading, Paused, PausedForPlayback,
                           WaitingForNetwork, Complete, Failed, Unauthorized,
                           ServerMissing, UpdateAvailable, LocalOnly, NoSpace };
enum class DownloadPlanState { Idle, Planning, Ready, Error };
enum class DownloadInterrupt { Playback, UserPause, Cancel, Shutdown, ScopeChanged };

// Pure state mapping shared by the worker and unit tests.  Cancel/shutdown and
// scope changes intentionally have no resumable state in the current scope.
inline DownloadState stateAfterInterrupt(DownloadInterrupt reason) {
    switch (reason) {
    case DownloadInterrupt::Playback: return DownloadState::PausedForPlayback;
    case DownloadInterrupt::UserPause: return DownloadState::Paused;
    default: return DownloadState::Failed;
    }
}

struct ByteRange { std::uint64_t start=0, end=0; bool valid=false; };
inline std::uint64_t chunkCount(std::uint64_t size, std::uint64_t chunkSize=DOWNLOAD_CHUNK_SIZE) {
    return !size || !chunkSize ? 0 : 1 + (size-1)/chunkSize;
}
inline ByteRange chunkRange(std::uint64_t size, std::uint64_t index,
                            std::uint64_t chunkSize=DOWNLOAD_CHUNK_SIZE) {
    if (!chunkSize || index >= chunkCount(size, chunkSize)) return {};
    const std::uint64_t start=index*chunkSize;
    return {start, (size-start < chunkSize ? size-1 : start+chunkSize-1), true};
}
inline std::uint64_t chunkLength(std::uint64_t size, std::uint64_t index,
                                 std::uint64_t chunkSize=DOWNLOAD_CHUNK_SIZE) {
    ByteRange r=chunkRange(size,index,chunkSize); return r.valid ? r.end-r.start+1 : 0;
}
inline std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) {
    return b > std::numeric_limits<std::uint64_t>::max()-a ? std::numeric_limits<std::uint64_t>::max() : a+b;
}
inline std::uint64_t saturatingMultiply(std::uint64_t a, std::uint64_t b) {
    return a && b>(std::numeric_limits<std::uint64_t>::max)()/a ? (std::numeric_limits<std::uint64_t>::max)() : a*b;
}
inline std::string formatBytes(std::uint64_t bytes) {
    char out[64]; const char *unit="B"; double n=(double)bytes;
    if (bytes >= 1024ULL*1024ULL*1024ULL) { n/=1024.0*1024.0*1024.0; unit="GB"; }
    else if (bytes >= 1024ULL*1024ULL) { n/=1024.0*1024.0; unit="MB"; }
    else if (bytes >= 1024ULL) { n/=1024.0; unit="KB"; }
    std::snprintf(out,sizeof(out), n>=10.0 || unit[0]=='B' ? "%.0f %s" : "%.1f %s",n,unit); return out;
}

struct DownloadItem {
    std::string itemId, itemType, title, seriesId, seriesName, seasonId, seasonName;
    std::string mediaSourceId, container, sourceEtag, videoCodec, audioCodec, lastError;
    // Last server source observed for a preserved local copy.  These remain
    // separate from the local source metadata used for chunk validation.
    std::string availableMediaSourceId, availableSourceEtag;
    std::uint64_t availableSize=0;
    std::int32_t seasonNumber=0, episodeNumber=0, width=0, height=0;
    std::int64_t runtimeTicks=0, playbackPositionTicks=0;
    std::uint64_t expectedSize=0, chunkSize=DOWNLOAD_CHUNK_SIZE, downloadedBytes=0, bitrate=0;
    // MFDM=2 stores independently playable HLS segments, not source byte ranges.
    bool hlsStorage=false;
    std::uint64_t hlsSegmentCount=0;
    std::string hlsProfile;
    // Runtime-only progress state. DownloadStore deliberately never serializes
    // these values: the completed segment count is reconstructed from files.
    std::uint64_t hlsCompletedSegments=0, hlsCurrentSegmentBytes=0,
                  hlsCurrentSegmentSize=0;
    unsigned hlsActivePercent=0;
    // Runtime-only; DownloadStore deliberately never serializes this value.
    std::uint64_t recentBytesPerSec=0;
    std::uint64_t createdAt=0, updatedAt=0;
    DownloadState state=DownloadState::Queued;
    bool localOnly=false, updateAvailable=false, externalSubtitles=false;
};
struct DownloadPlan { std::vector<DownloadItem> items; std::uint64_t totalSourceBytes=0, alreadyPresentBytes=0, additionalRequiredBytes=0, filesystemFreeBytes=0, alreadyReservedBytes=0, usableFreeBytes=0; bool sizeKnown=false, canFit=false; std::string error; };
struct DownloadPlanSnapshot { std::uint64_t id=0; DownloadPlanState state=DownloadPlanState::Idle; std::size_t itemCount=0; DownloadPlan plan; };
inline std::uint64_t remainingBytes(const DownloadItem &i) { return i.downloadedBytes >= i.expectedSize ? 0 : i.expectedSize-i.downloadedBytes; }
// Once an HLS playlist is known, project the unfinished segment work from
// actual segment bytes instead of retaining the conservative preflight size.
// The current segment's Content-Length, when available, is used directly.
inline std::uint64_t queueRemainingBytes(const DownloadItem &i) {
    if (i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly ||
        i.state==DownloadState::UpdateAvailable) return 0;
    if (!i.hlsStorage || !i.hlsSegmentCount) return remainingBytes(i);
    const std::uint64_t completed=std::min(i.hlsCompletedSegments,i.hlsSegmentCount);
    if (completed>=i.hlsSegmentCount) return 0;
    const std::uint64_t current=std::min(i.hlsCurrentSegmentBytes,i.downloadedBytes);
    const std::uint64_t completedBytes=i.downloadedBytes-current;
    const std::uint64_t remainingSegments=i.hlsSegmentCount-completed;
    if (i.hlsCurrentSegmentSize) {
        const std::uint64_t currentRemaining=i.hlsCurrentSegmentSize>current ? i.hlsCurrentSegmentSize-current : 0;
        if (remainingSegments==1) return currentRemaining;
        const std::uint64_t average=completed ? completedBytes/completed : i.hlsCurrentSegmentSize;
        return saturatingAdd(currentRemaining,saturatingMultiply(average,remainingSegments-1));
    }
    if (!completed) return 0;
    return saturatingMultiply(completedBytes/completed,remainingSegments);
}
inline bool plannedDownloadBytes(const DownloadItem &i, std::uint64_t &bytes) {
    if (i.hlsStorage) return estimateHlsBytes(i.runtimeTicks, bytes);
    bytes=i.expectedSize; return bytes!=0;
}
inline std::uint64_t displayDownloadBytes(const DownloadItem &i) {
    return i.hlsStorage && (i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly || i.state==DownloadState::UpdateAvailable) ? i.downloadedBytes : i.expectedSize;
}
inline const char *downloadStateLabel(DownloadState s) { switch(s) { case DownloadState::Queued:return "Queued"; case DownloadState::Planning:return "Planning"; case DownloadState::Downloading:return "Downloading"; case DownloadState::Paused:return "Paused"; case DownloadState::PausedForPlayback:return "Paused for playback"; case DownloadState::WaitingForNetwork:return "Waiting for network"; case DownloadState::Complete:return "Complete"; case DownloadState::Unauthorized:return "Unauthorized"; case DownloadState::ServerMissing:return "Server missing"; case DownloadState::UpdateAvailable:return "UPDATE AVAILABLE"; case DownloadState::LocalOnly:return "LOCAL ONLY"; case DownloadState::NoSpace:return "No space"; default:return "Failed"; } }
}
#endif
