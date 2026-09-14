#ifndef MIYOOFIN_DOWNLOAD_RECONCILE_HPP
#define MIYOOFIN_DOWNLOAD_RECONCILE_HPP
#include "DownloadTypes.hpp"
#include "../net/JellyfinApi.hpp"
namespace miyoofin {
enum class SourceCheck { Same, Changed, Missing, Transient, Unauthorized };
// Returns true when stale incomplete bytes must be removed before retrying.
bool reconcileSource(DownloadItem &item, SourceCheck result, const DownloadMediaSource *source=nullptr);
// True when the startup reconcile pass may skip a server source check for
// this item.  Completed local downloads are fully playable offline and do
// not need a PlaybackInfo POST at launch.
inline bool startupReconcileShouldSkip(const DownloadItem &item) {
    return item.state == DownloadState::Complete;
}
}
#endif
