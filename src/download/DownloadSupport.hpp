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
// Best-known total bytes for a download.  For HLS the server never publishes
// the transcode's final size, so once segments have completed predict it from
// the MEASURED average completed-segment size instead of the bitrate estimate.
inline std::uint64_t predictedDownloadTotalBytes(const DownloadItem &i) {
    // Completed items have an exact final size.
    if (i.state==DownloadState::Complete || i.state==DownloadState::LocalOnly ||
        i.state==DownloadState::UpdateAvailable)
        return i.downloadedBytes;
    // Non-HLS items: use expectedSize, never less than downloadedBytes.
    if (!i.hlsStorage) {
        return i.expectedSize >= i.downloadedBytes ? i.expectedSize : i.downloadedBytes;
    }
    // HLS with at least one completed segment: predict from measured average.
    if (i.hlsSegmentCount && i.hlsCompletedSegments >= 1) {
        const std::uint64_t current = std::min(i.hlsCurrentSegmentBytes, i.downloadedBytes);
        const std::uint64_t completedBytes = i.downloadedBytes >= current ? i.downloadedBytes - current : 0;
        if (i.hlsCompletedSegments) {
            const std::uint64_t avg = completedBytes / i.hlsCompletedSegments;
            if (avg) {
                const std::uint64_t predicted = saturatingMultiply(avg, i.hlsSegmentCount);
                // Never return less than what has already been written.
                return predicted >= i.downloadedBytes ? predicted : i.downloadedBytes;
}
        }
    }
    // HLS with 0 completed segments or degenerate input: fall back to the
    // estimate, never below what has already been written (all downloaded
    // bytes may belong to the in-progress segment, so avg==0 above).
    return i.expectedSize >= i.downloadedBytes ? i.expectedSize : i.downloadedBytes;
}
// Account for one newly completed HLS segment without rescanning segment
// files from disk.  Segment completions are sequential, so both counters are
// monotonic: a repeated completion for an already-accounted ordinal adds no
// bytes.  downloadedBytes is the exact on-disk total, never an estimate, so
// it is only saturating-added (never clamped to the preflight expectedSize,
// which may undershoot the real transcode).  Crash recovery does not depend
// on these counters: manifests never serialize hlsCompletedSegments and
// startup reconcile recomputes both counters from the segment files.
inline void applyHlsSegmentCompleted(DownloadItem &item, std::uint64_t segmentOrdinal,
                                      std::uint64_t segmentBytes) {
    const std::uint64_t completed = segmentOrdinal + 1;
    if (completed <= item.hlsCompletedSegments)
        return; // Already accounted; never double-count a retry.
    item.downloadedBytes = saturatingAdd(item.downloadedBytes, segmentBytes);
    item.hlsCompletedSegments = item.hlsSegmentCount
        ? std::min(completed, item.hlsSegmentCount) : completed;
}
// Publish transfer-owned progress onto the LIVE item without clobbering a
// concurrent interrupt.  Only playlist discovery and segment-progress fields
// are copied; Downloading is asserted solely when the live item is still
// Queued/Downloading.  A user pause or playback interrupt that landed between
// the segment rename and the lock acquisition therefore survives, so
// shouldAbort() keeps observing it and abort-on-pause still works.
inline void publishTransferProgress(DownloadItem &live, const DownloadItem &progress) {
    live.hlsStorage = progress.hlsStorage;
    live.hlsSegmentCount = progress.hlsSegmentCount;
    live.hlsProfile = progress.hlsProfile;
    live.expectedSize = progress.expectedSize;
    live.downloadedBytes = progress.downloadedBytes;
    live.hlsCompletedSegments = progress.hlsCompletedSegments;
    live.hlsCurrentSegmentBytes = progress.hlsCurrentSegmentBytes;
    live.hlsCurrentSegmentSize = progress.hlsCurrentSegmentSize;
    live.hlsActivePercent = std::max(progress.hlsActivePercent, downloadPercent(live));
    if (live.state == DownloadState::Queued || live.state == DownloadState::Downloading)
        live.state = DownloadState::Downloading;
}
// Source-identity guard for in-flight transfers.  A pause()/playback
// interrupt makes an item skippable for the reconciler while its transfer is
// still in flight (playlist HTTP / segment rename windows); when the server
// source changed, the reconciler resets the live item to Queued under the new
// identity.  The still-running transfer must then NOT republish its stale
// progress (which would force Queued->Downloading and mislabel old-source
// bytes as the new source) and must stop cleanly instead.
inline bool transferSourceChanged(const DownloadItem &live,
                                  const std::string &transferSourceId,
                                  const std::string &transferEtag) {
    return live.mediaSourceId != transferSourceId || live.sourceEtag != transferEtag;
}
// Completion-path outcome.  A consumed delete request wins over everything (a
// delete requested during the final segment removes instead of completing); a
// changed source aborts the stale transfer; a failed validation retries as a
// recoverable failure instead of stranding Downloading; otherwise Complete.
enum class TransferFinish { Complete, Removed, StaleAborted, RetryFailed };
inline TransferFinish decideTransferFinish(bool deleteRequested, bool sourceChanged,
                                           bool validated) {
    if (deleteRequested) return TransferFinish::Removed;
    if (sourceChanged) return TransferFinish::StaleAborted;
    if (!validated) return TransferFinish::RetryFailed;
    return TransferFinish::Complete;
}
// Recoverable validation-failure transition for the completion path: the item
// leaves Downloading (worker() only dequeues Queued, so Downloading would
// strand it) while a user pause / playback interrupt that landed during final
// validation is preserved, as on the segment-failure path.
inline void applyTransferValidationFailure(DownloadItem &live, const std::string &error) {
    if (live.state != DownloadState::Paused && live.state != DownloadState::PausedForPlayback)
        live.state = DownloadState::Failed;
    live.recentBytesPerSec = 0;
    live.lastError = error.empty() ? "validation failed" : error;
}
// statvfs under the download mutex on every snapshot stalls the UI thread
// while the Downloads tab polls.  Free space changes slowly, so snapshots
// reuse the cached value inside this TTL and only statvfs on expiry.
constexpr std::uint64_t kFreeSpaceCacheTtlMs = 3000;
inline bool freeSpaceCacheExpired(std::uint64_t nowMs, std::uint64_t cachedAtMs,
                                  std::uint64_t ttlMs = kFreeSpaceCacheTtlMs) {
    // No sample yet, or the clock jumped backwards: refresh.
    if (cachedAtMs == 0 || nowMs < cachedAtMs)
        return true;
    return nowMs - cachedAtMs >= ttlMs;
}
// A probe failure must not clobber a plausible cached value, but a
// successful probe of zero (genuinely full card) is authoritative and must
// be cached and returned as zero.  The caller substitutes the cached value
// ONLY when the probe actually failed.
inline std::uint64_t selectCachedFreeBytes(std::uint64_t cachedBytes, std::uint64_t freshBytes,
                                           bool probeOk) {
    if (probeOk)
        return freshBytes;
    return cachedBytes;
}
} // namespace miyoofin
#endif
