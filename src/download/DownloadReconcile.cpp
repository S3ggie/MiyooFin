#include "DownloadReconcile.hpp"
namespace miyoofin {
bool reconcileSource(DownloadItem &item, SourceCheck result, const DownloadMediaSource *source) {
    const bool complete=item.state==DownloadState::Complete || item.state==DownloadState::LocalOnly || item.state==DownloadState::UpdateAvailable;
    if (result==SourceCheck::Missing) { if (complete) { item.state=DownloadState::LocalOnly; item.localOnly=true; item.updateAvailable=false; } return false; }
    if (result==SourceCheck::Transient || result==SourceCheck::Unauthorized || !source) return false;
    const bool changed=item.mediaSourceId!=source->id || item.sourceEtag!=source->etag || (!item.hlsStorage && item.expectedSize!=source->size);
    if (!changed) { if (complete) { item.state=DownloadState::Complete; item.localOnly=false; item.updateAvailable=false; } return false; }
    if (complete) { item.state=DownloadState::UpdateAvailable; item.localOnly=false; item.updateAvailable=true; item.availableMediaSourceId=source->id; item.availableSourceEtag=source->etag; item.availableSize=source->size; return false; }
    item.mediaSourceId=source->id; item.sourceEtag=source->etag; if(item.hlsStorage){if(source->runtimeTicks)item.runtimeTicks=source->runtimeTicks;estimateHlsBytes(item.runtimeTicks,item.expectedSize);}else item.expectedSize=source->size; item.availableMediaSourceId.clear(); item.availableSourceEtag.clear(); item.availableSize=source->size; item.downloadedBytes=0; item.state=DownloadState::Queued; item.localOnly=false; item.updateAvailable=false; return true;
}
}
