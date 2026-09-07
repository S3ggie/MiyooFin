#ifndef MIYOOFIN_TELEMETRY_CONFIG_HPP
#define MIYOOFIN_TELEMETRY_CONFIG_HPP

#include <cstdint>
#include <string>

namespace miyoofin {

struct TelemetryConfig
{
    bool runtimeEnabled = false;
    uint32_t sampleIntervalMs = 1000;
    uint32_t freeSpaceIntervalMs = 10000;
    uint32_t writerBufferBytes = 32768;
    uint32_t flushIntervalMs = 5000;
    uint64_t rotateBytes = 16ull * 1024ull * 1024ull;
    uint32_t retainFiles = 4;
    uint64_t minFreeStorageBytes = 128ull * 1024ull * 1024ull;
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST)
    std::string targetDirectory = "/tmp/miyoofin-telemetry-test";
#else
    std::string targetDirectory = "/mnt/SDCARD/App/MiyooFin/telemetry-logs";
#endif

    static TelemetryConfig fromEnvironment();
};

#if !defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) || MIYOOFIN_ENABLE_PERF_TELEMETRY == 0
inline TelemetryConfig TelemetryConfig::fromEnvironment()
{
    return TelemetryConfig{};
}
#endif

} // namespace miyoofin

#endif // MIYOOFIN_TELEMETRY_CONFIG_HPP
