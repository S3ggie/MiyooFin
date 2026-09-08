#include "PerformanceTelemetry.hpp"

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

#include <algorithm>
#include <chrono>
#include <cstdint>
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

void PerformanceTelemetry::serviceLoop() noexcept
{
    m_serviceActive.store(true, std::memory_order_release);

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

    LinuxProcessMetrics metrics;
    const uint64_t sampleIntervalUs = std::max<uint64_t>(
        1ull, static_cast<uint64_t>(m_config.sampleIntervalMs) * 1000ull);
    const uint64_t freeSpaceIntervalUs = std::max<uint64_t>(
        1ull, static_cast<uint64_t>(m_config.freeSpaceIntervalMs) * 1000ull);
    uint64_t nextSampleUs = monotonicUs() + sampleIntervalUs;
    uint64_t nextFreeSpaceUs = monotonicUs();
    uint64_t lastFreeSpaceSampleUs = 0;
    uint64_t lastFreeStorageBytes = 0;
    bool haveFreeStorage = false;
    bool wasSamplingSuspended = false;

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
                LinuxProcessMetricsSnapshot snapshot = g_testHooks.processMetrics != nullptr
                    ? g_testHooks.processMetrics(refreshFreeSpace)
                    : metrics.sample(refreshFreeSpace);
#else
                LinuxProcessMetricsSnapshot snapshot = metrics.sample(refreshFreeSpace);
#endif
                if (refreshFreeSpace)
                    nextFreeSpaceUs = nowUs + freeSpaceIntervalUs;
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
