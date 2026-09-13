#pragma once
#include "PerformanceTelemetry.hpp"
#include "TelemetryClock.hpp"
#include "LinuxProcessMetrics.hpp"
#include <limits>

namespace miyoofin::telemetry_internal {
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
inline PerformanceTelemetry::TestHooks g_testHooks{};
inline uint16_t g_testSchemaVersion = 1;
#endif
inline uint64_t monotonicUs() noexcept {
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    if (g_testHooks.monotonicUs != nullptr) return g_testHooks.monotonicUs();
#endif
    return TelemetryClock::monotonicUs();
}
inline uint32_t clampToUint32(uint64_t value) noexcept { return value > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(value); }
inline void updateMaximum(std::atomic<uint32_t>& maximum, uint64_t value) noexcept { const uint32_t clamped=clampToUint32(value); uint32_t observed=maximum.load(std::memory_order_relaxed); while(clamped>observed&&!maximum.compare_exchange_weak(observed,clamped,std::memory_order_relaxed,std::memory_order_relaxed)){} }
inline LinuxProcessMetricsSnapshot sampleProcessMetrics(LinuxProcessMetrics &metrics, bool includeFreeStorage) noexcept {
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    return g_testHooks.processMetrics != nullptr ? g_testHooks.processMetrics(metrics, includeFreeStorage) : metrics.sample(includeFreeStorage);
#else
    return metrics.sample(includeFreeStorage);
#endif
}
}
