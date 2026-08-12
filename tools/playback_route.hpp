#ifndef MIYOOFIN_PLAYBACK_ROUTE_HPP
#define MIYOOFIN_PLAYBACK_ROUTE_HPP

#include <string>

// External playback has a canonical public identity and an optional verified
// LAN route.  Only transport failures may switch routes; an HTTP response is
// authoritative (especially 401/403).
struct PlaybackRoute {
    std::string primary;
    std::string fallback;
    bool usingLan = false;
};

inline PlaybackRoute playback_route(const std::string &publicUrl,
                                    const std::string &localUrl)
{
    return localUrl.empty() ? PlaybackRoute{publicUrl, {}, false}
                            : PlaybackRoute{localUrl, publicUrl, true};
}

inline bool playback_should_fallback(bool transportFailure, long httpStatus)
{
    return transportFailure && httpStatus == 0;
}

#endif
