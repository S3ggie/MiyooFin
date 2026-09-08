#ifndef MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
#define MIYOOFIN_PERFORMANCE_TELEMETRY_HPP

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
#include <atomic>
#include <thread>
#endif
#include <cstdint>

#include "TelemetryConfig.hpp"
#include "TelemetryIds.hpp"
#include "TelemetryRing.hpp"
#include "TelemetryTypes.hpp"
#include "TelemetryWriter.hpp"

namespace miyoofin {

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

class PerformanceTelemetry
{
public:
    void start(const TelemetryConfig &config);
    void stop() noexcept;
    bool enabledFast() const noexcept;
    void suspendSampling(bool suspended, SamplingReason reason = SamplingReason::Unknown) noexcept;
    uint64_t nextEphemeralId() noexcept;
    bool emitRecord(const TelemetryRecord &record) noexcept;
    bool emitSessionEvent(SessionEventKind kind,
                          Outcome outcome,
                          uint32_t value0,
                          uint64_t value1) noexcept;
    uint64_t droppedRecordCount() const noexcept;
    uint64_t writerErrorCount() const noexcept;
    uint32_t rotationCount() const noexcept;
    uint32_t samplingLateCount() const noexcept;
    bool serviceThreadActive() const noexcept;

private:
    void serviceLoop() noexcept;
    bool enqueue(TelemetryRecord record) noexcept;
    bool writeServiceRecord(TelemetryRecord record) noexcept;

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_samplingSuspended{false};
    std::atomic<uint64_t> m_nextEphemeralId{1};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_serviceActive{false};
    std::atomic<uint64_t> m_droppedRecords{0};
    std::atomic<uint64_t> m_writerErrors{0};
    std::atomic<uint32_t> m_rotationCount{0};
    std::atomic<uint32_t> m_samplingLate{0};
    TelemetryRing<512> m_ring;
    TelemetryWriter m_writer;
    std::thread m_serviceThread;
    uint32_t m_consumerSequence = 0;
    uint64_t m_sessionNonce = 0;
    TelemetryConfig m_config;
};

PerformanceTelemetry &performanceTelemetry() noexcept;

#else

class PerformanceTelemetry
{
public:
    inline void start(const TelemetryConfig &) noexcept {}
    inline void stop() noexcept {}
    inline bool enabledFast() const noexcept { return false; }
    inline void suspendSampling(bool, SamplingReason) noexcept {}
    inline uint64_t nextEphemeralId() noexcept { return 0; }
    inline bool emitRecord(const TelemetryRecord &) noexcept { return false; }
    inline bool emitSessionEvent(SessionEventKind, Outcome, uint32_t, uint64_t) noexcept { return false; }
    inline uint64_t droppedRecordCount() const noexcept { return 0; }
    inline uint64_t writerErrorCount() const noexcept { return 0; }
    inline uint32_t rotationCount() const noexcept { return 0; }
    inline uint32_t samplingLateCount() const noexcept { return 0; }
    inline bool serviceThreadActive() const noexcept { return false; }
};

inline PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

#endif

} // namespace miyoofin

#endif // MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
