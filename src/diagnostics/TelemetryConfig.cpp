#include "TelemetryConfig.hpp"

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

#include <cerrno>
#include <cstdlib>

namespace miyoofin {

TelemetryConfig TelemetryConfig::fromEnvironment()
{
    TelemetryConfig config;
    const char *enabled = std::getenv("MIYOOFIN_TELEMETRY");
    config.runtimeEnabled = enabled != nullptr && std::string(enabled) == "1";

    const char *minFree = std::getenv("MIYOOFIN_TELEMETRY_MIN_FREE_MIB");
    if (minFree != nullptr && *minFree != '\0') {
        bool digitsOnly = true;
        for (const char *cursor = minFree; *cursor != '\0'; ++cursor) {
            if (*cursor < '0' || *cursor > '9') {
                digitsOnly = false;
                break;
            }
        }
        if (digitsOnly) {
            errno = 0;
            char *end = nullptr;
            const unsigned long long parsed = std::strtoull(minFree, &end, 10);
            if (errno == 0 && end != minFree && *end == '\0') {
                const unsigned long long clamped = parsed < 64ull ? 64ull
                    : parsed > 4096ull ? 4096ull : parsed;
                config.minFreeStorageBytes = clamped * 1024ull * 1024ull;
            }
        }
    }

    return config;
}

} // namespace miyoofin

#endif
