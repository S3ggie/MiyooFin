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
// Single apply step shared by the reconciler worker and unit tests.  It
// operates on the LIVE item under the lock: the timestamp and the source
// merge run against *p directly, so a pause/playback transition that landed
// between the pass-start snapshot and the apply survives a Transient (no-op)
// reconcile.  Never assign a pass-start snapshot over the live element here.
inline bool applyReconciledSource(DownloadItem &live, SourceCheck result,
                                  const DownloadMediaSource *source,
                                  std::uint64_t nowMs) {
    live.lastVerifiedMs = nowMs;
    return reconcileSource(live, result, source);
}

// Session-level dedup constant: Complete items verified within this window
// are skipped during manual reconcile to avoid hammering the server on every
// Downloads tab entry.
constexpr std::uint64_t RECONCILE_VERIFY_TTL_MS = 24ULL * 60 * 60 * 1000;

// True when the reconcile loop may skip the PlaybackInfo source check for
// this item.  Startup pass: all Complete items.  Manual pass: Complete items
// whose lastVerifiedMs falls inside the TTL window.  Items never verified
// (lastVerifiedMs==0), non-Complete states, and explicit retries are always
// checked.
inline bool reconcileShouldSkip(const DownloadItem &item, bool startup,
                                std::uint64_t nowMs) {
    if (item.state != DownloadState::Complete)
        return false;
    if (startup)
        return true;
    return item.lastVerifiedMs > 0 &&
           (nowMs - item.lastVerifiedMs) < RECONCILE_VERIFY_TTL_MS;
}
}
#endif
