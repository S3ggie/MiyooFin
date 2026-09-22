#include "DownloadManager.hpp"
#include "../net/JellyfinApi.hpp"
#include "../net/RouteRequest.hpp"
#include "../net/TlsConfig.hpp"
#include "DownloadSupport.hpp"
#include "DownloadReconcile.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryGuards.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <cstdio>
#include <chrono>
#include <limits>
namespace miyoofin {
namespace {
struct WriteCtx
{
    FILE* f = nullptr;
    std::uint64_t remain = 0;
    DownloadManager* manager = nullptr;
    std::string itemId, scope;
    std::uint64_t generation = 0;
    std::uint64_t baseDownloaded = 0;
};
bool nonemptyFile(const std::string& path, std::uint64_t& size)
{
    struct stat st
    {};
    if (::stat(path.c_str(), &st) || st.st_size <= 0)
        return false;
    size = (std::uint64_t)st.st_size;
    return true;
}
size_t writeCb(char* p, size_t a, size_t b, void* u)
{
    auto* c = (WriteCtx*)u;
    std::uint64_t n = a * b;
    if (n > c->remain)
        return 0;
    size_t w = std::fwrite(p, 1, (size_t)n, c->f);
    c->remain -= w;
    return w;
}
int progressCb(void* u, curl_off_t total, curl_off_t, curl_off_t now, curl_off_t)
{
    auto* c = (WriteCtx*)u;
    if (!c->manager)
        return 1;
    auto ms = (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    c->manager->recordProgress(
        c->itemId, c->scope, c->generation, c->baseDownloaded + (now > 0 ? (std::uint64_t)now : 0),
        ms, now > 0 ? (std::uint64_t)now : 0, total > 0 ? (std::uint64_t)total : 0);
    return c->manager->shouldAbort(c->itemId, c->scope, c->generation);
}
void recordHlsSegmentAttempt(bool measured, std::uint64_t durationUs, std::uint32_t jobSequence,
                             std::uint64_t segmentOrdinal, unsigned attempt,
                             std::uint16_t retryDelayMs, RouteKind route, Outcome outcome,
                             bool retryPlanned, std::uint64_t payloadBytes, long httpStatus,
                             CURLcode curlCode) noexcept
{
    PerformanceTelemetry& telemetry = performanceTelemetry();
    if (!measured || !telemetry.enabledFast())
        return;
    TelemetryRecord record{};
    record.header.record_type = RecordType::DownloadSegmentAttempt;
    record.payload.download_segment_attempt.telemetry_job_seq = jobSequence;
    record.payload.download_segment_attempt.segment_ordinal =
        static_cast<std::uint32_t>(segmentOrdinal);
    record.payload.download_segment_attempt.attempt_number = static_cast<std::uint16_t>(attempt);
    record.payload.download_segment_attempt.retry_delay_ms = retryDelayMs;
    record.payload.download_segment_attempt.route_kind = static_cast<std::uint8_t>(route);
    record.payload.download_segment_attempt.outcome = static_cast<std::uint8_t>(outcome);
    record.payload.download_segment_attempt.retry_planned = retryPlanned ? 1u : 0u;
    record.payload.download_segment_attempt.duration_us = durationUs;
    record.payload.download_segment_attempt.payload_bytes = payloadBytes;
    record.payload.download_segment_attempt.http_status =
        static_cast<std::uint32_t>(httpStatus < 0 ? 0 : httpStatus);
    record.payload.download_segment_attempt.curl_code = static_cast<std::uint32_t>(curlCode);
    telemetry.emitRecord(record);
}
}
}
namespace miyoofin {
namespace {
void publishDownloadGauges(const std::vector<DownloadItem>& items, std::size_t plannerQueueDepth)
{
    std::uint32_t active = 0;
    std::uint32_t queued = 0;
    for (const auto& item : items) {
        if (item.state == DownloadState::Downloading)
            ++active;
        else if (item.state == DownloadState::Queued)
            ++queued;
    }
    performanceTelemetry().setDownloadGauges(active, queued,
                                             static_cast<std::uint32_t>(plannerQueueDepth));
}
}
DownloadState DownloadManager::hlsSegmentFailureState(long httpStatus, int curlCode)
{
    if (httpStatus == 401 || httpStatus == 403)
        return DownloadState::Unauthorized;
    if (httpStatus >= 400)
        return DownloadState::Failed;
    switch (curlCode) {
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return DownloadState::WaitingForNetwork;
    default:
        return DownloadState::Failed;
    }
}
bool DownloadManager::hlsSegmentRetryable(long httpStatus, int curlCode)
{
    switch (httpStatus) {
    case 500:
    case 502:
    case 503:
    case 504:
    case 520:
    case 521:
    case 522:
    case 523:
    case 524:
        return true;
    default:
        break;
    }
    if (httpStatus)
        return false;
    switch (curlCode) {
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
        return true;
    default:
        return false;
    }
}
bool DownloadManager::hlsSegmentShouldRetry(long httpStatus, int curlCode,
                                            unsigned completedAttempts)
{
    return completedAttempts < HLS_SEGMENT_ATTEMPTS && hlsSegmentRetryable(httpStatus, curlCode);
}
void DownloadManager::recordProgress(const std::string& id, const std::string& scope,
                                     std::uint64_t generation, std::uint64_t downloaded,
                                     std::uint64_t now, std::uint64_t currentBytes,
                                     std::uint64_t currentSize)
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (generation != m_generation || scope != m_scope)
        return;
    for (auto& i : m_items)
        if (i.itemId == id && i.state == DownloadState::Downloading) {
            if (!i.hlsStorage)
                downloaded = std::min(downloaded, i.expectedSize);
            const std::uint64_t previous = i.downloadedBytes;
            updateRecentSpeed(m_progressSamples[id], downloaded, now, i.recentBytesPerSec);
            i.downloadedBytes = downloaded;
            if (downloaded > previous)
                performanceTelemetry().addDownloadBytes(downloaded - previous);
            if (i.hlsStorage) {
                i.hlsCurrentSegmentBytes = currentBytes;
                i.hlsCurrentSegmentSize = currentSize;
                i.hlsActivePercent = downloadPercent(i);
            }
            return;
        }
}
bool DownloadManager::shouldAbort(const std::string& id, const std::string& scope,
                                  std::uint64_t generation) const
{
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_stop || m_playback || generation != m_generation || scope != m_scope)
        return true;
    for (const auto& i : m_items)
        if (i.itemId == id)
            return i.state != DownloadState::Downloading || m_deleteRequested.count(id);
    return true;
}
bool DownloadManager::waitForHlsSegmentRetry(const std::string& id, const std::string& scope,
                                             std::uint64_t generation, unsigned seconds)
{
    std::unique_lock<std::mutex> l(m_mutex);
    return !m_wake.wait_for(l, std::chrono::seconds(seconds), [&] {
        if (m_stop || m_playback || generation != m_generation || scope != m_scope)
            return true;
        for (const auto& i : m_items)
            if (i.itemId == id)
                return i.state != DownloadState::Downloading || m_deleteRequested.count(id);
        return true;
    });
}
void DownloadManager::worker()
{
    for (;;) {
        DownloadItem work;
        Session session;
        std::string scope;
        std::uint64_t generation = 0;
        bool persistOnly = false;
        {
            std::unique_lock<std::mutex> l(m_mutex);
            performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer, false);
            m_wake.wait(l, [&] {
                if (m_stop || m_persistRequested)
                    return true;
                if (!m_playback)
                    for (const auto& i : m_items)
                        if (i.state == DownloadState::Queued)
                            return true;
                return false;
            });
            if (m_stop)
                return;
            if (m_persistRequested) {
                scope = m_scope;
                generation = m_generation;
                persistOnly = true;
                // Id-set-change persist (enqueue() added ids, in-memory only on
                // the UI thread): manifests for ONLY the pending ids, each read
                // LIVE under its own short lock acquisition, plus one index write
                // (durable-manifest ids only, via saveIndexLocked) at the end.
                // The pending set is swapped under a
                // short hold and the mutex is released across file I/O, so every
                // hold spans a single small manifest/index write — a season-sized
                // enqueue never stalls SDL-thread state queries, pause(), or
                // enqueue() behind N fsyncs.  Read and write still share one acquisition
                // per item, so a pause()/erase()/playback-interrupt landing
                // before an item's write is written as-is, and one landing after
                // carries its own synchronous per-item persist that lands last —
                // either order converges, and a stale pass can never revert a
                // newer state.  A scope/generation change mid-pass (configure())
                // aborts the remainder: configure() already converged the whole
                // library under its own lock.
                std::set<std::string> pending;
                pending.swap(m_persistPendingIds);
                m_persistRequested = false;
                l.unlock();
                for (const auto& id : pending) {
                    std::lock_guard<std::mutex> pl(m_mutex);
                    if (m_stop || generation != m_generation || scope != m_scope)
                        break;
                    auto q = std::find_if(m_items.begin(), m_items.end(),
                                          [&](const DownloadItem& i) { return i.itemId == id; });
                    if (q == m_items.end())
                        continue;
                    if (q->hlsStorage)
                        m_store.ensureHlsDirectories(scope, q->itemId);
                    // An id joins the durable-manifest set only after its write
                    // succeeded; a later-enqueued id (or a failed write) stays
                    // out, so the index below can never name a manifestless id.
                    if (m_store.saveManifest(scope, *q, nullptr))
                        m_indexedIds.insert(id);
                }
                {
                    std::lock_guard<std::mutex> il(m_mutex);
                    if (!m_stop && generation == m_generation && scope == m_scope)
                        saveIndexLocked();
                }
                l.lock();
            } else {
                auto p = std::find_if(m_items.begin(), m_items.end(), [](const DownloadItem& i) {
                    return i.state == DownloadState::Queued;
                });
                if (p == m_items.end())
                    continue;
                p->state = DownloadState::Downloading;
                p->recentBytesPerSec = 0;
                m_progressSamples.erase(p->itemId);
                work = *p;
                session = m_session;
                scope = m_scope;
                generation = m_generation;
                publishDownloadGauges(m_items, m_planJobs.size());
                performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer, true);
            }
            // The dequeue flip changes no ids — the on-disk index already lists
            // this id (it is rewritten whenever the id set changes: the persist
            // pass above, erase, configure), so only the dequeued item's manifest
            // is rewritten here under the same lock acquisition as the state
            // change.  Bounded single-file I/O only: never network I/O, and no
            // lock order changes (m_mutex alone, as every other persist call site).
            if (!persistOnly) {
                if (work.hlsStorage)
                    m_store.ensureHlsDirectories(scope, work.itemId);
                persistItemLocked(work.itemId);
            }
        }
        if (persistOnly)
            continue;
        // Enqueue never performs removable-storage I/O: the worker above persists
        // before any transfer starts, so UI-thread enqueue stays in-memory only.
        transfer(work, session, scope, generation);
        performanceTelemetry().setWorkerActive(WorkerId::DownloadTransfer, false);
    }
}
bool DownloadManager::transfer(DownloadItem& item, const Session& session, const std::string& scope,
                               std::uint64_t generation)
{
    const std::uint32_t telemetryJobSequence =
        static_cast<std::uint32_t>(performanceTelemetry().nextEphemeralId());
    if (!session.valid())
        return false;
    // Source identity this transfer is working on (A): captured once after
    // the dequeue.  Every publish point below rechecks the live item under
    // the lock and aborts instead of republishing when the reconciler has
    // since reset the item to a new source.
    const std::string transferSourceId = item.mediaSourceId, transferEtag = item.sourceEtag;
    // A v1 payload is only retained when it is already complete.  New work is
    // always HLS and must never issue the original /Download request.
    if (!item.hlsStorage) {
        if (item.state == DownloadState::Complete &&
            m_store.validateCompletedDownload(scope, item, nullptr))
            return true;
        m_store.removePartialBytes(scope, item.itemId, nullptr);
        item.hlsStorage = true;
        item.hlsSegmentCount = 0;
        item.downloadedBytes = 0;
        estimateHlsBytes(item.runtimeTicks, item.expectedSize);
        item.hlsProfile = HLS_PROFILE_NAME;
    }
    std::vector<std::string> urls;
    std::string error;
    JellyfinApi::HlsFailure failure;
    if (!RouteRequest(session).run(
            [&](const std::string& base) {
                return JellyfinApi::getHlsSegmentUrls(base, session.accessToken, session.deviceId,
                                                      item.itemId, item.mediaSourceId, urls, error,
                                                      &failure);
            },
            error)) {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& i : m_items)
            if (i.itemId == item.itemId) {
                // A reconciler reset to a new source during discovery leaves Queued: it must not
                // take this stale failure state.  Preserve a pause/playback interrupt that landed
                // during playlist discovery: only a still-active item takes the failure state.
                if (transferSourceChanged(i, transferSourceId, transferEtag))
                    return false;
                if (i.state != DownloadState::Paused && i.state != DownloadState::PausedForPlayback)
                    i.state = failure == JellyfinApi::HlsFailure::Network
                                  ? DownloadState::WaitingForNetwork
                                  : (failure == JellyfinApi::HlsFailure::Unauthorized
                                         ? DownloadState::Unauthorized
                                         : DownloadState::Failed);
                i.recentBytesPerSec = 0;
                m_progressSamples.erase(i.itemId);
                i.lastError = error;
                persistItemLocked(i.itemId);
                publishDownloadGauges(m_items, m_planJobs.size());
            }
        return false;
    }
    item.hlsSegmentCount = urls.size();
    m_store.ensureHlsDirectories(scope, item.itemId);
    // Persist discovery before fetching segment zero: first-segment transcodes
    // may take a while, but the manifest must retain the discovered playlist.
    // A reconciler reset to a new source during discovery suppresses this
    // stale publish (same class as the guarded failure path above); the early
    // return also keeps the OLD-identity reconcile below from running, so it
    // can never write the stale identity back over the reset live item.  The
    // id set is unchanged by discovery, so only this item's manifest is
    // persisted (the index already lists this id).
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& i : m_items)
            if (i.itemId == item.itemId) {
                if (transferSourceChanged(i, transferSourceId, transferEtag))
                    return false;
                i.hlsSegmentCount = item.hlsSegmentCount;
                i.hlsStorage = true;
                // Recovery scan on the LIVE item (never the worker-local copy)
                // under this same lock: recompute resumed bytes from the
                // segment files on disk and persist the live manifest, so a
                // pause/reconciler-reset that landed before this point is
                // written as-is and never overwritten by stale local state.
                m_store.reconcile(scope, i, nullptr);
                item.downloadedBytes = i.downloadedBytes;
                break;
            }
    }
    // Recovery scan runs once per transfer; the resume offset is reused below
    // instead of scanning the segment directory a second time.
    const std::uint64_t resumeFrom = m_store.firstIncompleteSegment(scope, item);
    item.hlsCompletedSegments = resumeFrom;
    item.hlsCurrentSegmentBytes = item.hlsCurrentSegmentSize = 0;
    item.hlsActivePercent = downloadPercent(item);
    // Publish only transfer-owned progress onto the LIVE item: a pause or
    // playback interrupt that landed after the dequeue must survive instead
    // of being reverted to Downloading by a stale whole-item assignment.
    {
        std::lock_guard<std::mutex> l(m_mutex);
        for (auto& i : m_items)
            if (i.itemId == item.itemId) {
                if (transferSourceChanged(i, transferSourceId, transferEtag))
                    return false;
                publishTransferProgress(i, item);
                persistItemLocked(i.itemId);
                break;
            }
    }
    for (std::uint64_t k = resumeFrom; k < urls.size(); ++k) {
        // Consume an abandoned delete flag exactly like the segment-failure
        // path: a delete requested then left pending must remove now instead
        // of surviving to silently delete a later re-enqueue of this id.
        // Delete still wins over pause/playback/generation aborts.
        if (shouldAbort(item.itemId, scope, generation)) {
            std::lock_guard<std::mutex> l(m_mutex);
            if (m_deleteRequested.erase(item.itemId)) {
                auto p = std::find_if(m_items.begin(), m_items.end(), [&](const DownloadItem& i) {
                    return i.itemId == item.itemId;
                });
                if (p != m_items.end()) {
                    m_store.removeItem(scope, item.itemId, nullptr);
                    m_items.erase(p);
                    m_progressSamples.erase(item.itemId);
                    m_indexedIds.erase(item.itemId);
                    m_persistPendingIds.erase(item.itemId);
                } // Id set changed: index only.
                saveIndexLocked();
                publishDownloadGauges(m_items, m_planJobs.size());
            }
            return false;
        }
        std::string part = m_store.segmentPath(scope, item.itemId, k, true),
                    done = m_store.segmentPath(scope, item.itemId, k);
        CURLcode rc = CURLE_OK;
        long code = 0;
        std::uint64_t bytes = 0;
        bool good = false, primaryGood = false, fallbackGood = false;
        std::uint64_t primaryDuration = 0, primaryBytes = 0, fallbackDuration = 0,
                      fallbackBytes = 0;
        long primaryCode = 0, fallbackCode = 0;
        CURLcode primaryRc = CURLE_OK, fallbackRc = CURLE_OK;
        bool primaryMeasured = false, fallbackMeasured = false, fallbackAttempted = false;
        for (unsigned attempt = 1; attempt <= HLS_SEGMENT_ATTEMPTS; ++attempt) {
            primaryDuration = primaryBytes = fallbackDuration = fallbackBytes = 0;
            primaryCode = fallbackCode = 0;
            primaryRc = fallbackRc = CURLE_OK;
            primaryMeasured = fallbackMeasured = fallbackAttempted = false;
            primaryGood = fallbackGood = false;
            // "wb" deliberately replaces a failed response body before every retry.
            FILE* f = std::fopen(part.c_str(), "wb");
            if (!f) {
                rc = CURLE_WRITE_ERROR;
                break;
            }
            CURL* c = curl_easy_init();
            if (!c) {
                std::fclose(f);
                rc = CURLE_FAILED_INIT;
                break;
            }
            auto headers = JellyfinApi::buildAuthHeaders(session.accessToken, session.deviceId);
            curl_slist* sl = nullptr;
            for (auto& h : headers)
                sl = curl_slist_append(sl, h.c_str());
            WriteCtx ctx{f,
                         std::numeric_limits<std::uint64_t>::max(),
                         this,
                         item.itemId,
                         scope,
                         generation,
                         item.downloadedBytes};
            std::string segmentUrl = urls[k];
            const RouteRequest routes(session);
            const std::string lanUrl = routes.lan(), publicBase = routes.publicRoute();
            const RouteKind primaryRoute =
                (!lanUrl.empty() && segmentUrl.compare(0, lanUrl.size(), lanUrl) == 0)
                    ? RouteKind::Lan
                    : RouteKind::Public;
            curl_easy_setopt(c, CURLOPT_URL, segmentUrl.c_str());
            curl_easy_setopt(c, CURLOPT_HTTPHEADER, sl);
            curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
            curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
            std::string tlsError;
            if (!configureTls(c, segmentUrl, &tlsError)) {
                rc = CURLE_SSL_CACERT;
                std::fclose(f);
                curl_slist_free_all(sl);
                curl_easy_cleanup(c);
                break;
            }
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 64L);
            curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 180L);
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, writeCb);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
            curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progressCb);
            curl_easy_setopt(c, CURLOPT_XFERINFODATA, &ctx);
            TelemetryRouteScope primaryRouteScope(primaryRoute, static_cast<std::uint8_t>(attempt),
                                                  false);
            TelemetryTimer primaryTimer;
            rc = curl_easy_perform(c);
            primaryDuration = primaryTimer.elapsedUs();
            primaryMeasured = primaryTimer.active();
            code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            std::fflush(f);
            std::fclose(f);
            curl_slist_free_all(sl);
            curl_easy_cleanup(c);
            good = rc == CURLE_OK && code >= 200 && code < 300 && nonemptyFile(part, bytes);
            primaryRc = rc;
            primaryCode = code;
            primaryBytes = good ? bytes : 0;
            primaryGood = good;
            if (!good && rc != CURLE_OK && !lanUrl.empty() && !publicBase.empty() &&
                segmentUrl.compare(0, lanUrl.size(), lanUrl) == 0) {
                fallbackAttempted = true;
                std::printf("[Route] LAN failed; public fallback\n");
                std::remove(part.c_str());
                FILE* fallback = std::fopen(part.c_str(), "wb");
                if (fallback) {
                    CURL* pc = curl_easy_init();
                    if (pc) {
                        curl_slist* ps = nullptr;
                        for (const auto& h :
                             JellyfinApi::buildAuthHeaders(session.accessToken, session.deviceId))
                            ps = curl_slist_append(ps, h.c_str());
                        WriteCtx pctx{fallback,
                                      std::numeric_limits<std::uint64_t>::max(),
                                      this,
                                      item.itemId,
                                      scope,
                                      generation,
                                      item.downloadedBytes};
                        std::string publicUrl =
                            RouteRequest::replaceBase(segmentUrl, lanUrl, publicBase);
                        curl_easy_setopt(pc, CURLOPT_URL, publicUrl.c_str());
                        curl_easy_setopt(pc, CURLOPT_HTTPHEADER, ps);
                        curl_easy_setopt(pc, CURLOPT_FOLLOWLOCATION, 1L);
                        curl_easy_setopt(pc, CURLOPT_NOSIGNAL, 1L);
                        curl_easy_setopt(pc, CURLOPT_CONNECTTIMEOUT, 15L);
                        std::string tlsError;
                        if (!configureTls(pc, publicUrl, &tlsError)) {
                            rc = CURLE_SSL_CACERT;
                            curl_slist_free_all(ps);
                            curl_easy_cleanup(pc);
                        } else {
                            curl_easy_setopt(pc, CURLOPT_WRITEFUNCTION, writeCb);
                            curl_easy_setopt(pc, CURLOPT_WRITEDATA, &pctx);
                            curl_easy_setopt(pc, CURLOPT_NOPROGRESS, 0L);
                            curl_easy_setopt(pc, CURLOPT_XFERINFOFUNCTION, progressCb);
                            curl_easy_setopt(pc, CURLOPT_XFERINFODATA, &pctx);
                            TelemetryRouteScope fallbackRouteScope(
                                RouteKind::Public, static_cast<std::uint8_t>(attempt), true);
                            TelemetryTimer fallbackTimer;
                            rc = curl_easy_perform(pc);
                            fallbackDuration = fallbackTimer.elapsedUs();
                            fallbackMeasured = fallbackTimer.active();
                            code = 0;
                            curl_easy_getinfo(pc, CURLINFO_RESPONSE_CODE, &code);
                            fallbackRc = rc;
                            fallbackCode = code;
                            curl_slist_free_all(ps);
                            curl_easy_cleanup(pc);
                        }
                    }
                    std::fflush(fallback);
                    std::fclose(fallback);
                    good = rc == CURLE_OK && code >= 200 && code < 300 && nonemptyFile(part, bytes);
                }
            }
            if (fallbackAttempted) {
                fallbackGood = good;
                fallbackBytes = good ? bytes : 0;
            }
            bool retry = false;
            if (!good && rc != CURLE_ABORTED_BY_CALLBACK)
                retry = hlsSegmentShouldRetry(code, (int)rc, attempt);
            const std::uint16_t retryDelayMs =
                retry ? static_cast<std::uint16_t>((1u << (attempt - 1)) * 1000u) : 0;
            recordHlsSegmentAttempt(primaryMeasured, primaryDuration, telemetryJobSequence, k,
                                    attempt, retryDelayMs, primaryRoute,
                                    primaryGood ? Outcome::Success
                                                : (primaryRc == CURLE_ABORTED_BY_CALLBACK
                                                       ? Outcome::Cancelled
                                                       : Outcome::Failure),
                                    retry, primaryBytes, primaryCode, primaryRc);
            if (fallbackAttempted)
                recordHlsSegmentAttempt(fallbackMeasured, fallbackDuration, telemetryJobSequence, k,
                                        attempt, retryDelayMs, RouteKind::Public,
                                        fallbackGood ? Outcome::Success
                                                     : (fallbackRc == CURLE_ABORTED_BY_CALLBACK
                                                            ? Outcome::Cancelled
                                                            : Outcome::Failure),
                                        retry, fallbackBytes, fallbackCode, fallbackRc);
            if (good) {
                std::printf("[Download] segment=%llu attempt=%u HTTP=%ld\n", (unsigned long long)k,
                            attempt, code);
                break;
            }
            if (rc == CURLE_ABORTED_BY_CALLBACK)
                break;
            std::printf("[Download] segment=%llu attempt=%u HTTP=%ld%s\n", (unsigned long long)k,
                        attempt, code, retry ? " retrying" : "");
            if (!retry)
                break;
            const bool willRetry =
                waitForHlsSegmentRetry(item.itemId, scope, generation, 1u << (attempt - 1));
            if (willRetry)
                performanceTelemetry().addDownloadSegmentRetries();
            if (!willRetry)
                break;
        }
        if (!good) {
            std::string detail = "segment=" + std::to_string(k) +
                                 " curl=" + std::to_string((int)rc) + " " + curl_easy_strerror(rc) +
                                 " HTTP=" + std::to_string(code);
            std::lock_guard<std::mutex> l(m_mutex);
            auto p = std::find_if(m_items.begin(), m_items.end(),
                                  [&](const DownloadItem& i) { return i.itemId == item.itemId; });
            if (p == m_items.end())
                return false;
            if (m_deleteRequested.erase(item.itemId)) {
                m_store.removeItem(scope, item.itemId, nullptr);
                m_items.erase(p);
                m_progressSamples.erase(item.itemId);
                m_indexedIds.erase(item.itemId);
                m_persistPendingIds.erase(item.itemId);
                saveIndexLocked(); // Id set changed: index only.
                publishDownloadGauges(m_items, m_planJobs.size());
                return false;
            }
            if (rc == CURLE_ABORTED_BY_CALLBACK) {
                p->recentBytesPerSec = 0;
                m_progressSamples.erase(item.itemId);
                m_store.reconcile(scope, *p, nullptr);
                publishDownloadGauges(m_items, m_planJobs.size());
                return false;
            }
            if (p->state != DownloadState::Paused && p->state != DownloadState::PausedForPlayback)
                p->state = hlsSegmentFailureState(code, (int)rc);
            p->recentBytesPerSec = 0;
            m_progressSamples.erase(item.itemId);
            p->lastError = detail;
            m_store.reconcile(scope, *p, nullptr);
            publishDownloadGauges(m_items, m_planJobs.size());
            return false;
        }
        if (std::rename(part.c_str(), done.c_str()))
            return false;
        performanceTelemetry().addDownloadSegmentCompleted();
        // The segment file just renamed to its final name is the source of
        // truth: account for its bytes incrementally instead of rescanning
        // every segment from disk, and persist only this item's manifest.
        // The index (item ids) is unchanged by segment progress, and a crash
        // between segments still recovers: startup reconcile rebuilds the
        // counters from the segment files on disk.
        applyHlsSegmentCompleted(item, k, bytes);
        item.hlsCurrentSegmentBytes = item.hlsCurrentSegmentSize = 0;
        item.hlsActivePercent = downloadPercent(item);
        {
            std::lock_guard<std::mutex> l(m_mutex);
            for (auto& i : m_items)
                if (i.itemId == item.itemId) {
                    // The reconciler already wiped this source's segment files when it
                    // reset the identity above; drop the just-renamed stale file inside
                    // this same critical section (never after unlock, never twice) so a
                    // concurrently restarted new-source transfer cannot rename a valid
                    // segment onto this path in between, then stop cleanly.
                    if (transferSourceChanged(i, transferSourceId, transferEtag)) {
                        std::remove(done.c_str());
                        return false;
                    }
                    publishTransferProgress(i, item);
                    persistItemLocked(i.itemId);
                    break;
                }
        }
    }
    std::lock_guard<std::mutex> l(m_mutex);
    auto p = std::find_if(m_items.begin(), m_items.end(),
                          [&](const DownloadItem& i) { return i.itemId == item.itemId; });
    if (p == m_items.end())
        return false;
    // The delete flag is consumed here as on the failure path: a delete
    // requested during the final segment removes instead of completing (B).
    const bool deleteRequested = m_deleteRequested.erase(item.itemId) != 0;
    // Identity this transfer ran under vs the live item (A); validation only
    // runs when neither delete nor a source change already decided it (C).
    const bool sourceChanged =
        !deleteRequested && transferSourceChanged(*p, transferSourceId, transferEtag);
    std::string validationError;
    const bool validated = !deleteRequested && !sourceChanged &&
                           m_store.validateCompletedDownload(scope, item, &validationError);
    switch (decideTransferFinish(deleteRequested, sourceChanged, validated)) {
    case TransferFinish::Removed:
        m_store.removeItem(scope, item.itemId, nullptr);
        m_items.erase(p);
        m_progressSamples.erase(item.itemId);
        m_indexedIds.erase(item.itemId);
        m_persistPendingIds.erase(item.itemId);
        saveIndexLocked(); // Id set changed: index only.
        publishDownloadGauges(m_items, m_planJobs.size());
        return false;
    case TransferFinish::StaleAborted:
        return false;
    case TransferFinish::RetryFailed:
        applyTransferValidationFailure(*p, validationError);
        m_progressSamples.erase(p->itemId);
        m_store.reconcile(scope, *p, nullptr);
        publishDownloadGauges(m_items, m_planJobs.size());
        return false;
    case TransferFinish::Complete:
        break;
    }
    // No reconcile() of the worker-local copy here: validation already ran
    // against the segment files on disk, and completion is published
    // field-wise onto the live item with a single manifest write below, so no
    // stale local state ever reaches disk.
    item.state = DownloadState::Complete;
    item.recentBytesPerSec = 0;
    item.lastError.clear();
    // Disk-validated completion is terminal truth, so Complete is published even over an interrupt;
    // still field-wise, never a stale whole-item copy.
    p->hlsStorage = item.hlsStorage;
    p->hlsSegmentCount = item.hlsSegmentCount;
    p->hlsProfile = item.hlsProfile;
    p->expectedSize = item.expectedSize;
    p->downloadedBytes = item.downloadedBytes;
    p->hlsCompletedSegments = item.hlsCompletedSegments;
    p->hlsCurrentSegmentBytes = 0;
    p->hlsCurrentSegmentSize = 0;
    p->hlsActivePercent = item.hlsActivePercent;
    p->state = DownloadState::Complete;
    p->recentBytesPerSec = 0;
    p->lastError.clear();
    m_progressSamples.erase(p->itemId);
    m_store.saveManifest(scope, *p, nullptr);
    publishDownloadGauges(m_items, m_planJobs.size());
    return true;
}
}
