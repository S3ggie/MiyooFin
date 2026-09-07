#ifndef MIYOOFIN_TELEMETRY_GUARDS_HPP
#define MIYOOFIN_TELEMETRY_GUARDS_HPP

#include <cstdint>

#include "TelemetryClock.hpp"
#include "TelemetryContext.hpp"

namespace miyoofin {

#if defined(MIYOOFIN_ENABLE_PERF_TELEMETRY) && MIYOOFIN_ENABLE_PERF_TELEMETRY == 1

class TelemetryRequestScope
{
public:
    explicit TelemetryRequestScope(RequestKind request) noexcept
    {
        if (!performanceTelemetry().enabledFast())
            return;
        m_active = true;
        m_previous = telemetryContext().requestKind;
        telemetryContext().requestKind = request;
    }

    ~TelemetryRequestScope()
    {
        if (m_active)
            telemetryContext().requestKind = m_previous;
    }

private:
    bool m_active = false;
    RequestKind m_previous = RequestKind::Unknown;
};

class TelemetryRouteScope
{
public:
    TelemetryRouteScope(RouteKind route, uint8_t attempt, bool fallback) noexcept
    {
        if (!performanceTelemetry().enabledFast())
            return;
        m_active = true;
        m_previousRoute = telemetryContext().routeKind;
        m_previousAttempt = telemetryContext().routeAttempt;
        m_previousFallback = telemetryContext().routeFallback;
        telemetryContext().routeKind = route;
        telemetryContext().routeAttempt = attempt;
        telemetryContext().routeFallback = fallback;
    }

    ~TelemetryRouteScope()
    {
        if (!m_active)
            return;
        telemetryContext().routeKind = m_previousRoute;
        telemetryContext().routeAttempt = m_previousAttempt;
        telemetryContext().routeFallback = m_previousFallback;
    }

private:
    bool m_active = false;
    RouteKind m_previousRoute = RouteKind::Unknown;
    uint8_t m_previousAttempt = 0;
    bool m_previousFallback = false;
};

class TelemetryArtworkScope
{
public:
    explicit TelemetryArtworkScope(ArtworkContext artwork) noexcept
    {
        if (!performanceTelemetry().enabledFast())
            return;
        m_active = true;
        m_previous = telemetryContext().artworkContext;
        telemetryContext().artworkContext = artwork;
    }

    ~TelemetryArtworkScope()
    {
        if (m_active)
            telemetryContext().artworkContext = m_previous;
    }

private:
    bool m_active = false;
    ArtworkContext m_previous = ArtworkContext::Unknown;
};

class TelemetryTimer
{
public:
    TelemetryTimer() noexcept
    {
        if (!performanceTelemetry().enabledFast())
            return;
        m_active = true;
        m_startUs = TelemetryClock::monotonicUs();
    }

    bool active() const noexcept { return m_active; }

    uint64_t elapsedUs() const noexcept
    {
        if (!m_active)
            return 0;
        const uint64_t nowUs = TelemetryClock::monotonicUs();
        return nowUs >= m_startUs ? nowUs - m_startUs : 0;
    }

private:
    bool m_active = false;
    uint64_t m_startUs = 0;
};

#else

class TelemetryRequestScope
{
public:
    explicit TelemetryRequestScope(RequestKind) noexcept {}
};

class TelemetryRouteScope
{
public:
    TelemetryRouteScope(RouteKind, uint8_t, bool) noexcept {}
};

class TelemetryArtworkScope
{
public:
    explicit TelemetryArtworkScope(ArtworkContext) noexcept {}
};

class TelemetryTimer
{
public:
    TelemetryTimer() noexcept {}
    bool active() const noexcept { return false; }
    uint64_t elapsedUs() const noexcept { return 0; }
};

#endif

} // namespace miyoofin

#endif // MIYOOFIN_TELEMETRY_GUARDS_HPP
