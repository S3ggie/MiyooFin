#include "DownloadSupport.hpp"
#include <algorithm>
namespace miyoofin {
DownloadItem makeDownloadItem(const MediaItem &m,const DownloadMediaSource&s,const MediaItem *series,const MediaItem *season) {
    DownloadItem i; i.itemId=m.id;i.itemType=m.type;i.title=m.title;i.seriesId=!m.seriesId.empty()?m.seriesId:(series?series->id:"");
    i.seriesName=!m.seriesName.empty()?m.seriesName:(series?series->title:"");i.seasonId=!m.seasonId.empty()?m.seasonId:(season?season->id:"");
    i.seasonName=season?season->title:"";i.seasonNumber=m.parentIndexNumber?m.parentIndexNumber:(season?season->indexNumber:0);i.episodeNumber=m.indexNumber;
    i.mediaSourceId=s.id;i.container=s.container;i.sourceEtag=s.etag;i.videoCodec=s.videoCodec;i.audioCodec=s.audioCodec;i.width=s.width;i.height=s.height;i.bitrate=s.bitrate;i.runtimeTicks=s.runtimeTicks?s.runtimeTicks:m.runTimeTicks;i.playbackPositionTicks=m.playbackPositionTicks;i.hlsStorage=true;i.hlsProfile=HLS_PROFILE_NAME;i.externalSubtitles=s.hasExternalSubtitles; estimateHlsBytes(i.runtimeTicks,i.expectedSize); return i;
}
PlaybackSource resolvePlayback(const MediaItem &item,const DownloadManager &d,bool offline) { if(d.hasComplete(item.id)) return PlaybackSource::Local; return offline?PlaybackSource::UnavailableOffline:PlaybackSource::Jellyfin; }
}
