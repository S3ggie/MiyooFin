#include "PerformanceTelemetry.hpp"

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <thread>
#include <unistd.h>

#include "TelemetryClock.hpp"
#include "miyoofin/version.hpp"

namespace miyoofin {
namespace {

#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
PerformanceTelemetry::TestHooks g_testHooks{};
#endif

uint64_t monotonicUs() noexcept
{
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    if (g_testHooks.monotonicUs != nullptr)
        return g_testHooks.monotonicUs();
#endif
    return TelemetryClock::monotonicUs();
}

uint32_t clampToUint32(uint64_t value) noexcept
{
    return value > std::numeric_limits<uint32_t>::max()
        ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(value);
}

LinuxProcessMetricsSnapshot sampleProcessMetrics(LinuxProcessMetrics &metrics,
                                                 bool includeFreeStorage) noexcept
{
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    return g_testHooks.processMetrics != nullptr
        ? g_testHooks.processMetrics(includeFreeStorage)
        : metrics.sample(includeFreeStorage);
#else
    return metrics.sample(includeFreeStorage);
#endif
}

} // namespace

#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
void PerformanceTelemetry::setTestHooks(const TestHooks &hooks) noexcept
{
    g_testHooks = hooks;
}

void PerformanceTelemetry::clearTestHooks() noexcept
{
    g_testHooks = TestHooks{};
}
#endif

void PerformanceTelemetry::start(const TelemetryConfig &config)
{
    stop();
    m_config = config;
    m_samplingSuspended.store(false, std::memory_order_relaxed);
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_droppedRecords.store(0, std::memory_order_relaxed);
    m_writerErrors.store(0, std::memory_order_relaxed);
    m_rotationCount.store(0, std::memory_order_relaxed);
    m_samplingLate.store(0, std::memory_order_relaxed);
    m_queueHighwater.store(0, std::memory_order_relaxed);
    for (WorkerSlot &slot : m_workerSlots) {
        slot.active.store(0, std::memory_order_relaxed);
        slot.queueDepth.store(0, std::memory_order_relaxed);
        slot.queueHighwater.store(0, std::memory_order_relaxed);
        slot.completed.store(0, std::memory_order_relaxed);
        slot.failed.store(0, std::memory_order_relaxed);
        slot.cancelled.store(0, std::memory_order_relaxed);
    }
    m_artworkCacheProbeHits.store(0, std::memory_order_relaxed);
    m_artworkCacheProbeMisses.store(0, std::memory_order_relaxed);
    m_artworkCacheReadSuccess.store(0, std::memory_order_relaxed);
    m_artworkCacheReadFailure.store(0, std::memory_order_relaxed);
    m_artworkCacheWriteSuccess.store(0, std::memory_order_relaxed);
    m_artworkCacheWriteFailure.store(0, std::memory_order_relaxed);
    m_artworkCompressedReadBytes.store(0, std::memory_order_relaxed);
    m_artworkCompressedWrittenBytes.store(0, std::memory_order_relaxed);
    m_artworkDecodeCount.store(0, std::memory_order_relaxed);
    m_artworkDecodeFailures.store(0, std::memory_order_relaxed);
    m_artworkDecodeTotalUs.store(0, std::memory_order_relaxed);
    m_artworkDecodeMaxUs.store(0, std::memory_order_relaxed);
    m_activeDownloads.store(0, std::memory_order_relaxed);
    m_queuedDownloads.store(0, std::memory_order_relaxed);
    m_plannerQueueDepth.store(0, std::memory_order_relaxed);
    m_downloadBytes.store(0, std::memory_order_relaxed);
    m_downloadSegmentsCompleted.store(0, std::memory_order_relaxed);
    m_downloadSegmentRetries.store(0, std::memory_order_relaxed);
    m_consumerSequence = 0;
    m_sessionNonce = monotonicUs()
        ^ (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(this)) * 0x9e3779b97f4a7c15ull)
        ^ static_cast<uint64_t>(::getpid());
    if (m_sessionNonce == 0)
        m_sessionNonce = 1;
    if (!config.runtimeEnabled)
        return;

    m_enabled.store(true, std::memory_order_release);
    m_serviceThread = std::thread(&PerformanceTelemetry::serviceLoop, this);
}

void PerformanceTelemetry::stop() noexcept
{
    m_enabled.store(false, std::memory_order_release);
    m_samplingSuspended.store(false, std::memory_order_relaxed);
    m_stopRequested.store(true, std::memory_order_release);
    if (m_serviceThread.joinable())
        m_serviceThread.join();
}

bool PerformanceTelemetry::enabledFast() const noexcept
{
    return m_enabled.load(std::memory_order_relaxed);
}

PerformanceTelemetry::WorkerSlot *PerformanceTelemetry::workerSlot(WorkerId worker) noexcept
{
    const uint16_t value = static_cast<uint16_t>(worker);
    if (value == 0 || value > m_workerSlots.size())
        return nullptr;
    return &m_workerSlots[value - 1];
}

void PerformanceTelemetry::setWorkerActive(WorkerId worker, bool active) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->active.store(active ? 1u : 0u, std::memory_order_relaxed);
}

void PerformanceTelemetry::updateQueueHighwater(uint32_t depth) noexcept
{
    uint32_t observed = m_queueHighwater.load(std::memory_order_relaxed);
    while (depth > observed
        && !m_queueHighwater.compare_exchange_weak(observed, depth,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
    }
}

void PerformanceTelemetry::setWorkerQueueDepth(WorkerId worker, uint32_t depth) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker)) {
        slot->queueDepth.store(depth, std::memory_order_relaxed);
        uint32_t observed = slot->queueHighwater.load(std::memory_order_relaxed);
        while (depth > observed
            && !slot->queueHighwater.compare_exchange_weak(observed, depth,
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
        }
    }
}

void PerformanceTelemetry::addWorkerCompleted(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->completed.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceTelemetry::addWorkerFailed(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->failed.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceTelemetry::addWorkerCancelled(WorkerId worker, uint32_t count) noexcept
{
    if (!enabledFast())
        return;
    if (WorkerSlot *slot = workerSlot(worker))
        slot->cancelled.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceTelemetry::recordArtworkCacheProbe(bool hit) noexcept
{
    if (!enabledFast())
        return;
    (hit ? m_artworkCacheProbeHits : m_artworkCacheProbeMisses)
        .fetch_add(1, std::memory_order_relaxed);
}

void PerformanceTelemetry::recordArtworkCacheRead(bool success, uint64_t compressedBytes) noexcept
{
    if (!enabledFast())
        return;
    if (success) {
        m_artworkCacheReadSuccess.fetch_add(1, std::memory_order_relaxed);
        m_artworkCompressedReadBytes.fetch_add(compressedBytes, std::memory_order_relaxed);
    } else {
        m_artworkCacheReadFailure.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceTelemetry::recordArtworkCacheWrite(bool success, uint64_t compressedBytes) noexcept
{
    if (!enabledFast())
        return;
    if (success) {
        m_artworkCacheWriteSuccess.fetch_add(1, std::memory_order_relaxed);
        m_artworkCompressedWrittenBytes.fetch_add(compressedBytes, std::memory_order_relaxed);
    } else {
        m_artworkCacheWriteFailure.fetch_add(1, std::memory_order_relaxed);
    }
}

void PerformanceTelemetry::recordArtworkDecode(bool success, uint64_t durationUs) noexcept
{
    if (!enabledFast())
        return;
    m_artworkDecodeCount.fetch_add(1, std::memory_order_relaxed);
    if (!success)
        m_artworkDecodeFailures.fetch_add(1, std::memory_order_relaxed);
    m_artworkDecodeTotalUs.fetch_add(durationUs, std::memory_order_relaxed);
    uint32_t observed = m_artworkDecodeMaxUs.load(std::memory_order_relaxed);
    const uint32_t duration = clampToUint32(durationUs);
    while (duration > observed
        && !m_artworkDecodeMaxUs.compare_exchange_weak(observed, duration,
                                                        std::memory_order_relaxed,
                                                        std::memory_order_relaxed)) {
    }
}

void PerformanceTelemetry::setDownloadGauges(uint32_t activeDownloads,
                                             uint32_t queuedDownloads,
                                             uint32_t plannerQueueDepth) noexcept
{
    if (!enabledFast())
        return;
    m_activeDownloads.store(activeDownloads, std::memory_order_relaxed);
    m_queuedDownloads.store(queuedDownloads, std::memory_order_relaxed);
    m_plannerQueueDepth.store(plannerQueueDepth, std::memory_order_relaxed);
}

void PerformanceTelemetry::addDownloadBytes(uint64_t bytes) noexcept
{
    if (enabledFast())
        m_downloadBytes.fetch_add(bytes, std::memory_order_relaxed);
}

void PerformanceTelemetry::addDownloadSegmentCompleted(uint32_t count) noexcept
{
    if (enabledFast())
        m_downloadSegmentsCompleted.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceTelemetry::addDownloadSegmentRetries(uint32_t count) noexcept
{
    if (enabledFast())
        m_downloadSegmentRetries.fetch_add(count, std::memory_order_relaxed);
}

void PerformanceTelemetry::suspendSampling(bool suspended, SamplingReason reason) noexcept
{
    (void)reason;
    if (enabledFast())
        m_samplingSuspended.store(suspended, std::memory_order_relaxed);
}

uint64_t PerformanceTelemetry::nextEphemeralId() noexcept
{
    if (!enabledFast())
        return 0;
    return m_nextEphemeralId.fetch_add(1, std::memory_order_relaxed);
}

bool PerformanceTelemetry::emitRecord(const TelemetryRecord &record) noexcept
{
    if (!enabledFast())
        return false;
    TelemetryRecord copy = record;
    if (copy.header.monotonic_us == 0)
        copy.header.monotonic_us = monotonicUs();
    return enqueue(copy);
}

bool PerformanceTelemetry::emitSessionEvent(SessionEventKind kind,
                                             Outcome outcome,
                                             uint32_t value0,
                                             uint64_t value1) noexcept
{
    if (!enabledFast())
        return false;
    TelemetryRecord record{};
    record.header.record_type = RecordType::SessionEvent;
    record.header.monotonic_us = monotonicUs();
    record.payload.session_event.kind = static_cast<uint8_t>(kind);
    record.payload.session_event.outcome = static_cast<uint8_t>(outcome);
    record.payload.session_event.value0 = value0;
    record.payload.session_event.value1 = value1;
    return enqueue(record);
}

bool PerformanceTelemetry::enqueue(TelemetryRecord record) noexcept
{
    if (!m_ring.tryEnqueue(record)) {
        m_droppedRecords.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    updateQueueHighwater(m_ring.approximateDepth());
    return true;
}

bool PerformanceTelemetry::writeServiceRecord(TelemetryRecord record) noexcept
{
    record.header.sequence = ++m_consumerSequence;
    if (record.header.monotonic_us == 0)
        record.header.monotonic_us = monotonicUs();
    if (!m_writer.append(record)) {
        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    m_rotationCount.store(m_writer.rotationCount(), std::memory_order_relaxed);
    return true;
}

void PerformanceTelemetry::emitWorkerSamples(uint64_t nowUs) noexcept
{
    for (std::size_t index = 0; index < m_workerSlots.size(); ++index) {
        WorkerSlot &slot = m_workerSlots[index];
        const uint32_t depth = slot.queueDepth.load(std::memory_order_relaxed);
        uint32_t highwater = slot.queueHighwater.exchange(0, std::memory_order_relaxed);
        if (highwater < depth)
            highwater = depth;
        TelemetryRecord record{};
        record.header.record_type = RecordType::WorkerSample;
        record.header.monotonic_us = nowUs;
        record.payload.worker_sample.worker_id = static_cast<uint16_t>(index + 1);
        record.payload.worker_sample.active = static_cast<uint8_t>(
            slot.active.load(std::memory_order_relaxed) != 0);
        record.payload.worker_sample.queue_depth = depth;
        record.payload.worker_sample.queue_highwater = highwater;
        record.payload.worker_sample.completed_delta = slot.completed.exchange(0, std::memory_order_relaxed);
        record.payload.worker_sample.failed_delta = slot.failed.exchange(0, std::memory_order_relaxed);
        record.payload.worker_sample.cancelled_delta = slot.cancelled.exchange(0, std::memory_order_relaxed);
        writeServiceRecord(record);
    }
}

void PerformanceTelemetry::emitArtworkSummary(uint64_t nowUs) noexcept
{
    TelemetryRecord record{};
    record.header.record_type = RecordType::ArtworkSummary;
    record.header.monotonic_us = nowUs;
    record.payload.artwork_summary.cache_probe_hits = m_artworkCacheProbeHits.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.cache_probe_misses = m_artworkCacheProbeMisses.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.cache_read_success = m_artworkCacheReadSuccess.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.cache_read_failure = m_artworkCacheReadFailure.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.cache_write_success = m_artworkCacheWriteSuccess.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.cache_write_failure = m_artworkCacheWriteFailure.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.compressed_read_bytes = m_artworkCompressedReadBytes.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.compressed_written_bytes = m_artworkCompressedWrittenBytes.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.decode_count = m_artworkDecodeCount.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.decode_failures = m_artworkDecodeFailures.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.decode_total_us = m_artworkDecodeTotalUs.exchange(0, std::memory_order_relaxed);
    record.payload.artwork_summary.decode_max_us = m_artworkDecodeMaxUs.exchange(0, std::memory_order_relaxed);
    writeServiceRecord(record);
}

void PerformanceTelemetry::emitDownloadSample(uint64_t nowUs, uint64_t actualIntervalUs) noexcept
{
    const uint64_t bytesDelta = m_downloadBytes.exchange(0, std::memory_order_relaxed);
    uint64_t bytesPerSecond = 0;
    if (bytesDelta <= std::numeric_limits<uint64_t>::max() / 1000000ull)
        bytesPerSecond = (bytesDelta * 1000000ull) / std::max<uint64_t>(1, actualIntervalUs);
    else
        bytesPerSecond = std::numeric_limits<uint64_t>::max();

    TelemetryRecord record{};
    record.header.record_type = RecordType::DownloadSample;
    record.header.monotonic_us = nowUs;
    record.payload.download_sample.active_downloads = m_activeDownloads.load(std::memory_order_relaxed);
    record.payload.download_sample.queued_downloads = m_queuedDownloads.load(std::memory_order_relaxed);
    record.payload.download_sample.bytes_delta = bytesDelta;
    record.payload.download_sample.bytes_per_sec = bytesPerSecond;
    record.payload.download_sample.segments_completed_delta = m_downloadSegmentsCompleted.exchange(0, std::memory_order_relaxed);
    record.payload.download_sample.segment_retries_delta = m_downloadSegmentRetries.exchange(0, std::memory_order_relaxed);
    record.payload.download_sample.planner_queue_depth = m_plannerQueueDepth.load(std::memory_order_relaxed);
    writeServiceRecord(record);
}

void PerformanceTelemetry::emitTelemetryHealth(uint64_t nowUs) noexcept
{
    const uint32_t depth = m_ring.approximateDepth();
    uint32_t highwater = m_queueHighwater.exchange(0, std::memory_order_relaxed);
    if (highwater < depth)
        highwater = depth;
    TelemetryRecord record{};
    record.header.record_type = RecordType::TelemetryHealth;
    record.header.monotonic_us = nowUs;
    record.payload.telemetry_health.queue_depth = depth;
    record.payload.telemetry_health.queue_highwater = highwater;
    record.payload.telemetry_health.dropped_records_cumulative = clampToUint32(
        m_droppedRecords.load(std::memory_order_relaxed));
    record.payload.telemetry_health.writer_errors_cumulative = clampToUint32(
        m_writerErrors.load(std::memory_order_relaxed));
    record.payload.telemetry_health.rotation_count = m_rotationCount.load(std::memory_order_relaxed);
    record.payload.telemetry_health.sampling_late_count = m_samplingLate.load(std::memory_order_relaxed);
    record.payload.telemetry_health.buffered_bytes = clampToUint32(m_writer.bufferedBytes());
    writeServiceRecord(record);
}

void PerformanceTelemetry::serviceLoop() noexcept
{
    LinuxProcessMetrics metrics;
    const LinuxProcessMetricsSnapshot startupSnapshot = sampleProcessMetrics(metrics, true);
    if ((startupSnapshot.validity_flags & FreeStorageValid) != 0
        && startupSnapshot.free_storage_bytes < m_config.minFreeStorageBytes) {
        std::fprintf(stderr, "[telemetry] disabled: insufficient free storage\n");
        m_enabled.store(false, std::memory_order_release);
        m_stopRequested.store(true, std::memory_order_release);
        return;
    }

    MftFileHeader header{};
    header.flags = 2u;
    header.pid = static_cast<uint32_t>(::getpid());
    header.session_nonce = m_sessionNonce;
    header.start_monotonic_us = monotonicUs();
    header.sample_interval_ms = m_config.sampleIntervalMs;
    header.free_space_interval_ms = m_config.freeSpaceIntervalMs;
    header.rotation_index = 0;
    header.writer_buffer_bytes = m_config.writerBufferBytes;
    header.app_version_packed = (static_cast<uint32_t>(VERSION_MAJOR) << 16)
        | (static_cast<uint32_t>(VERSION_MINOR) << 8)
        | static_cast<uint32_t>(VERSION_PATCH);
#if defined(__arm__)
    header.platform_id = static_cast<uint32_t>(PlatformId::MiyooMiniPlusOnionOS);
#else
    header.platform_id = static_cast<uint32_t>(PlatformId::Host);
#endif

    if (!m_writer.open(m_config, header)) {
        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
        m_enabled.store(false, std::memory_order_release);
        m_serviceActive.store(false, std::memory_order_release);
        return;
    }

    TelemetryRecord started{};
    started.header.record_type = RecordType::SessionEvent;
    started.header.monotonic_us = monotonicUs();
    started.payload.session_event.kind = static_cast<uint8_t>(SessionEventKind::TelemetryStarted);
    started.payload.session_event.outcome = static_cast<uint8_t>(Outcome::Success);
    started.payload.session_event.value0 = m_config.sampleIntervalMs;
    started.payload.session_event.value1 = m_config.minFreeStorageBytes;
    writeServiceRecord(started);

    const uint64_t sampleIntervalUs = std::max<uint64_t>(
        1ull, static_cast<uint64_t>(m_config.sampleIntervalMs) * 1000ull);
    const uint64_t freeSpaceIntervalUs = std::max<uint64_t>(
        1ull, static_cast<uint64_t>(m_config.freeSpaceIntervalMs) * 1000ull);
    uint64_t nextSampleUs = monotonicUs() + sampleIntervalUs;
    uint64_t nextFreeSpaceUs = monotonicUs();
    uint64_t lastFreeSpaceSampleUs = 0;
    uint64_t lastFreeStorageBytes = 0;
    uint64_t lastAggregateSampleUs = 0;
    bool haveFreeStorage = false;
    bool wasSamplingSuspended = false;
    m_serviceActive.store(true, std::memory_order_release);

    TelemetryRecord record{};
    while (!m_stopRequested.load(std::memory_order_acquire)
        || m_ring.approximateDepth() != 0) {
        bool drained = false;
        while (m_ring.tryDequeue(record)) {
            drained = true;
            writeServiceRecord(record);
        }
        const bool stopping = m_stopRequested.load(std::memory_order_acquire);
        const uint64_t nowUs = monotonicUs();
        if (!stopping && m_samplingSuspended.load(std::memory_order_relaxed)) {
            wasSamplingSuspended = true;
        } else if (!stopping && wasSamplingSuspended) {
            nextSampleUs = nowUs + sampleIntervalUs;
            nextFreeSpaceUs = nowUs + freeSpaceIntervalUs;
            wasSamplingSuspended = false;
        } else if (!stopping && nowUs >= nextSampleUs) {
            if (nowUs - nextSampleUs > sampleIntervalUs) {
                m_samplingLate.fetch_add(1, std::memory_order_relaxed);
                nextSampleUs = nowUs + sampleIntervalUs;
            } else {
                const bool refreshFreeSpace = nowUs >= nextFreeSpaceUs;
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
                LinuxProcessMetricsSnapshot snapshot = sampleProcessMetrics(metrics, refreshFreeSpace);
#else
                LinuxProcessMetricsSnapshot snapshot = metrics.sample(refreshFreeSpace);
#endif
                if (refreshFreeSpace)
                    nextFreeSpaceUs = nowUs + freeSpaceIntervalUs;
                if (refreshFreeSpace
                    && (snapshot.validity_flags & FreeStorageValid) != 0
                    && snapshot.free_storage_bytes < m_config.minFreeStorageBytes) {
                    TelemetryRecord disabled{};
                    disabled.header.record_type = RecordType::SessionEvent;
                    disabled.header.monotonic_us = nowUs;
                    disabled.payload.session_event.kind = static_cast<uint8_t>(
                        SessionEventKind::WriterDisabledLowSpace);
                    disabled.payload.session_event.outcome = static_cast<uint8_t>(Outcome::Skipped);
                    disabled.payload.session_event.value0 = clampToUint32(
                        m_config.minFreeStorageBytes / (1024ull * 1024ull));
                    disabled.payload.session_event.value1 = snapshot.free_storage_bytes;
                    writeServiceRecord(disabled);
                    emitTelemetryHealth(nowUs);
                    if (!m_writer.flush())
                        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
                    if (!m_writer.close())
                        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
                    m_enabled.store(false, std::memory_order_release);
                    m_stopRequested.store(true, std::memory_order_release);
                    m_serviceActive.store(false, std::memory_order_release);
                    return;
                }
                if ((snapshot.validity_flags & FreeStorageValid) != 0) {
                    lastFreeStorageBytes = snapshot.free_storage_bytes;
                    lastFreeSpaceSampleUs = nowUs;
                    haveFreeStorage = true;
                }
                if (haveFreeStorage) {
                    snapshot.free_storage_bytes = lastFreeStorageBytes;
                    snapshot.validity_flags |= FreeStorageValid;
                } else {
                    snapshot.free_storage_bytes = 0;
                    snapshot.validity_flags &= ~FreeStorageValid;
                }

                TelemetryRecord systemSample{};
                systemSample.header.record_type = RecordType::SystemSample;
                systemSample.header.monotonic_us = nowUs;
                systemSample.payload.system_sample.process_cpu_us_cumulative =
                    snapshot.process_cpu_us_cumulative;
                systemSample.payload.system_sample.rss_kib = snapshot.rss_kib;
                systemSample.payload.system_sample.peak_rss_kib = snapshot.peak_rss_kib;
                systemSample.payload.system_sample.process_read_bytes_cumulative =
                    snapshot.process_read_bytes_cumulative;
                systemSample.payload.system_sample.process_write_bytes_cumulative =
                    snapshot.process_write_bytes_cumulative;
                systemSample.payload.system_sample.free_storage_bytes =
                    snapshot.free_storage_bytes;
                systemSample.payload.system_sample.telemetry_logical_bytes_written =
                    m_writer.logicalBytesWritten();
                systemSample.payload.system_sample.dropped_records_cumulative =
                    clampToUint32(m_droppedRecords.load(std::memory_order_relaxed));
                const uint64_t freeAgeUs = haveFreeStorage && nowUs >= lastFreeSpaceSampleUs
                    ? nowUs - lastFreeSpaceSampleUs : 0;
                systemSample.payload.system_sample.free_storage_sample_age_ms =
                    clampToUint32(freeAgeUs / 1000ull);
                systemSample.payload.system_sample.validity_flags = snapshot.validity_flags;
                writeServiceRecord(systemSample);
                const uint64_t actualIntervalUs = lastAggregateSampleUs != 0
                    && nowUs > lastAggregateSampleUs
                    ? nowUs - lastAggregateSampleUs : sampleIntervalUs;
                emitWorkerSamples(nowUs);
                emitArtworkSummary(nowUs);
                emitDownloadSample(nowUs, actualIntervalUs);
                emitTelemetryHealth(nowUs);
                lastAggregateSampleUs = nowUs;
                nextSampleUs = nowUs + sampleIntervalUs;
            }
        }
        if (!m_writer.flushIfDue(nowUs))
            m_writerErrors.fetch_add(1, std::memory_order_relaxed);
        if (!drained && !stopping)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    TelemetryRecord stopped{};
    stopped.header.record_type = RecordType::SessionEvent;
    stopped.header.monotonic_us = monotonicUs();
    stopped.payload.session_event.kind = static_cast<uint8_t>(SessionEventKind::TelemetryStopped);
    stopped.payload.session_event.outcome = static_cast<uint8_t>(Outcome::Success);
    stopped.payload.session_event.value1 = m_writer.logicalBytesWritten();
    writeServiceRecord(stopped);
    if (!m_writer.flush())
        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
    if (!m_writer.close())
        m_writerErrors.fetch_add(1, std::memory_order_relaxed);
    m_rotationCount.store(m_writer.rotationCount(), std::memory_order_relaxed);
    m_serviceActive.store(false, std::memory_order_release);
}

uint64_t PerformanceTelemetry::droppedRecordCount() const noexcept
{
    return m_droppedRecords.load(std::memory_order_relaxed);
}

uint64_t PerformanceTelemetry::writerErrorCount() const noexcept
{
    return m_writerErrors.load(std::memory_order_relaxed);
}

uint32_t PerformanceTelemetry::rotationCount() const noexcept
{
    return m_rotationCount.load(std::memory_order_relaxed);
}

uint32_t PerformanceTelemetry::samplingLateCount() const noexcept
{
    return m_samplingLate.load(std::memory_order_relaxed);
}

bool PerformanceTelemetry::serviceThreadActive() const noexcept
{
    return m_serviceActive.load(std::memory_order_acquire);
}

PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

} // namespace miyoofin

#endif
