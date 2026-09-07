#ifndef MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
#define MIYOOFIN_PERFORMANCE_TELEMETRY_HPP

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1
#include <atomic>
#endif
#include <cstdint>

#include "TelemetryConfig.hpp"
#include "TelemetryIds.hpp"

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

private:
    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_samplingSuspended{false};
    std::atomic<uint64_t> m_nextEphemeralId{1};
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
};

inline PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

#endif

} // namespace miyoofin

#endif // MIYOOFIN_PERFORMANCE_TELEMETRY_HPP
