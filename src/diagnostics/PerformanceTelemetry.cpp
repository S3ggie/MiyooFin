#include "PerformanceTelemetry.hpp"

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

namespace miyoofin {

void PerformanceTelemetry::start(const TelemetryConfig &config)
{
    m_config = config;
    m_samplingSuspended.store(false, std::memory_order_relaxed);
    m_enabled.store(config.runtimeEnabled, std::memory_order_relaxed);
}

void PerformanceTelemetry::stop() noexcept
{
    m_enabled.store(false, std::memory_order_relaxed);
    m_samplingSuspended.store(false, std::memory_order_relaxed);
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

PerformanceTelemetry &performanceTelemetry() noexcept
{
    static PerformanceTelemetry instance;
    return instance;
}

} // namespace miyoofin

#endif
