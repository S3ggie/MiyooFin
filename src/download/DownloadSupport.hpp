#ifndef MIYOOFIN_DOWNLOAD_SUPPORT_HPP
#define MIYOOFIN_DOWNLOAD_SUPPORT_HPP

#include "DownloadTypes.hpp"
#include "DownloadManager.hpp"
#include "../data/MediaItem.hpp"
#include "../net/JellyfinApi.hpp"

namespace miyoofin {
DownloadItem makeDownloadItem(const MediaItem &item, const DownloadMediaSource &source,
                              const MediaItem *series=nullptr, const MediaItem *season=nullptr);
enum class PlaybackSource { Local, Jellyfin, UnavailableOffline };
PlaybackSource resolvePlayback(const MediaItem &item, const DownloadManager &downloads,
                              bool networkKnownOffline=false);
inline std::string episodeDownloadLabel(const DownloadItem &i) {
    char b[32]; std::snprintf(b,sizeof(b),"S%02dE%02d ",i.seasonNumber,i.episodeNumber);
    return i.itemType == "episode" ? std::string(b) + i.title : i.title;
}
inline unsigned downloadPercent(const DownloadItem &i) {
    if (i.hlsStorage) {
        if (i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly ||
            i.state==DownloadState::UpdateAvailable) return 100;
        if (!i.hlsSegmentCount) return 0;
        const std::uint64_t completed=std::min(i.hlsCompletedSegments,i.hlsSegmentCount);
        long double progress=(long double)completed/(long double)i.hlsSegmentCount;
        // libcurl only supplies a useful fraction when the segment length is known.
        if (completed<i.hlsSegmentCount && i.hlsCurrentSegmentSize && i.hlsCurrentSegmentBytes)
            progress+=(long double)std::min(i.hlsCurrentSegmentBytes,i.hlsCurrentSegmentSize)/
                      (long double)i.hlsCurrentSegmentSize/(long double)i.hlsSegmentCount;
        unsigned percent=(unsigned)(progress*100.0L);
        // Completion is shown only after all segments have been validated.
        percent=std::min(99u,percent);
        return std::min(100u,std::max(percent,i.hlsActivePercent));
    }
    return i.expectedSize ? (unsigned)std::min<std::uint64_t>(100, i.downloadedBytes*100/i.expectedSize) : 0;
}
}
#endif
