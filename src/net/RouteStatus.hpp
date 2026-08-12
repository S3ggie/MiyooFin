#ifndef MIYOOFIN_ROUTE_STATUS_HPP
#define MIYOOFIN_ROUTE_STATUS_HPP

#include <atomic>
#include <cstdint>

namespace miyoofin {

enum class ApiRoute : std::uint8_t { Unknown, Lan, Public };

// Process-local diagnostic state for the last successful normal app/API call.
class RouteStatus {
public:
    static ApiRoute latest()
    {
        return static_cast<ApiRoute>(s_latest.load(std::memory_order_acquire));
    }
    static void record(ApiRoute route)
    {
        s_latest.store(static_cast<std::uint8_t>(route), std::memory_order_release);
    }
    static const char *label(ApiRoute route)
    {
        switch (route) {
        case ApiRoute::Lan: return "LAN";
        case ApiRoute::Public: return "PUBLIC";
        case ApiRoute::Unknown: return "UNKNOWN";
        }
        return "UNKNOWN";
    }
private:
    inline static std::atomic<std::uint8_t> s_latest{
        static_cast<std::uint8_t>(ApiRoute::Unknown)};
};

}
#endif
