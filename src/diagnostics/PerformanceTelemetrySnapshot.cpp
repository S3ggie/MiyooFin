#include "PerformanceTelemetry.hpp"
#include "PerformanceTelemetryInternal.hpp"

namespace miyoofin {
using namespace telemetry_internal;

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
}
