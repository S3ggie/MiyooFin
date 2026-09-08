#ifndef MIYOOFIN_ROUTE_REQUEST_HPP
#define MIYOOFIN_ROUTE_REQUEST_HPP

#include "Session.hpp"
#include "ServerAddress.hpp"
#include "RouteStatus.hpp"
#if defined(MIYOOFIN_TELEMETRY_HOST_TEST) \
    && !defined(MIYOOFIN_TELEMETRY_RING_TEST)
#define MIYOOFIN_ROUTE_REQUEST_RING_TEST 1
#define MIYOOFIN_TELEMETRY_RING_TEST 1
#endif
#include "../diagnostics/TelemetryGuards.hpp"
#if defined(MIYOOFIN_ROUTE_REQUEST_RING_TEST)
#undef MIYOOFIN_TELEMETRY_RING_TEST
#undef MIYOOFIN_ROUTE_REQUEST_RING_TEST
#endif
#include <cstdio>
#include <string>

namespace miyoofin {

// Keeps persistent identity separate from the endpoint used for one request.
// Operations must return false only for failures and place transport failures
// in error with the HttpClient's "Transport: " prefix.
class RouteRequest {
public:
    explicit RouteRequest(const Session &session) : m_session(session) {}
    std::string lan() const {
        return !m_session.localServerUrl.empty() ? m_session.localServerUrl :
            (isObviousLanServerUrl(m_session.serverUrl) ? m_session.serverUrl : "");
    }
    std::string publicRoute() const {
        return isObviousLanServerUrl(m_session.serverUrl) ? m_session.publicServerUrl : m_session.serverUrl;
    }
    std::string primary() const { const std::string local=lan(); return local.empty() ? publicRoute() : local; }
    bool usesLan() const { return !lan().empty(); }
    static bool transportFailure(const std::string &error) { return error.compare(0, 11, "Transport: ") == 0 && error.find("Callback aborted") == std::string::npos; }

    template <typename Operation>
    bool run(Operation operation, std::string &error) const {
        const std::string local=lan();
        const std::string publicUrl=publicRoute();
        if (local.empty()) {
            std::printf("[Route] PUBLIC\n");
            const bool succeeded=[&] {
                TelemetryRouteScope route(RouteKind::Public, 1, false);
                return operation(publicUrl);
            }();
            if (succeeded) RouteStatus::record(ApiRoute::Public);
            return succeeded;
        }
        std::printf("[Route] LAN\n");
        const bool succeeded=[&] {
            TelemetryRouteScope route(RouteKind::Lan, 1, false);
            return operation(local);
        }();
        if (succeeded) {
            RouteStatus::record(ApiRoute::Lan);
            return true;
        }
        if (!transportFailure(error) || publicUrl.empty() || publicUrl==local) return false;
        std::printf("[Route] LAN failed; public fallback\n");
        error.clear();
        std::printf("[Route] PUBLIC\n");
        const bool fallbackSucceeded=[&] {
            TelemetryRouteScope route(RouteKind::Public, 2, true);
            return operation(publicUrl);
        }();
        if (fallbackSucceeded) RouteStatus::record(ApiRoute::Public);
        return fallbackSucceeded;
    }

    static std::string replaceBase(const std::string &url, const std::string &from, const std::string &to) {
        return url.compare(0, from.size(), from)==0 ? to+url.substr(from.size()) : url;
    }
private:
    const Session &m_session;
};
}
#endif
