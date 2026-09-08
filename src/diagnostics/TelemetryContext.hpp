#ifndef MIYOOFIN_TELEMETRY_CONTEXT_HPP
#define MIYOOFIN_TELEMETRY_CONTEXT_HPP

#include <cstdint>

#include "PerformanceTelemetry.hpp"

namespace miyoofin {

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

struct TelemetryContext
{
    RequestKind requestKind = RequestKind::Unknown;
    RouteKind routeKind = RouteKind::Unknown;
    uint8_t routeAttempt = 0;
    bool routeFallback = false;
    ArtworkContext artworkContext = ArtworkContext::Unknown;
};

inline thread_local TelemetryContext g_telemetryContext;

inline TelemetryContext &telemetryContext() noexcept
{
    return g_telemetryContext;
}

inline RequestKind currentRequestKind() noexcept
{
    if (!performanceTelemetry().enabledFast())
        return RequestKind::Unknown;
    return telemetryContext().requestKind;
}

inline RouteKind currentRouteKind() noexcept
{
    if (!performanceTelemetry().enabledFast())
        return RouteKind::Unknown;
    return telemetryContext().routeKind;
}

inline uint8_t currentRouteAttempt() noexcept
{
    if (!performanceTelemetry().enabledFast())
        return 0;
    return telemetryContext().routeAttempt;
}

inline bool currentRouteFallback() noexcept
{
    if (!performanceTelemetry().enabledFast())
        return false;
    return telemetryContext().routeFallback;
}

inline ArtworkContext currentArtworkContext() noexcept
{
    if (!performanceTelemetry().enabledFast())
        return ArtworkContext::Unknown;
    return telemetryContext().artworkContext;
}

#else

inline RequestKind currentRequestKind() noexcept { return RequestKind::Unknown; }
inline RouteKind currentRouteKind() noexcept { return RouteKind::Unknown; }
inline uint8_t currentRouteAttempt() noexcept { return 0; }
inline bool currentRouteFallback() noexcept { return false; }
inline ArtworkContext currentArtworkContext() noexcept { return ArtworkContext::Unknown; }

#endif

} // namespace miyoofin

#endif // MIYOOFIN_TELEMETRY_CONTEXT_HPP
