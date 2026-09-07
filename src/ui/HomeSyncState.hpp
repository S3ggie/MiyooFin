#ifndef MIYOOFIN_HOME_SYNC_STATE_HPP
#define MIYOOFIN_HOME_SYNC_STATE_HPP

#include <SDL2/SDL.h>
#include <algorithm>
#include <cstddef>
#include <string>

namespace miyoofin {
struct LibrarySyncSchedule {
    static constexpr Uint32 FRESH_MS = 15u * 60u * 1000u;
    static constexpr Uint32 RETRY_DELAY_MS = 60u * 1000u;
    bool inFlight=false, pending=false, hasAttempted=false, hasSucceeded=false;
    Uint32 lastAttempt=0, lastSuccess=0;
    bool request(Uint32 now) {
        if (inFlight) { pending=true; return false; }
        if (hasSucceeded && now-lastSuccess < FRESH_MS) return false;
        if (hasAttempted && now-lastAttempt < RETRY_DELAY_MS) return false;
        inFlight=true; hasAttempted=true; lastAttempt=now; return true;
    }
    // A completed request already includes all coalesced navigation requests.
    // Never immediately retry a failed hostname/network request on the UI path.
    bool complete(Uint32 now, bool succeeded) {
        inFlight=false; pending=false;
        if (succeeded) { hasSucceeded=true; lastSuccess=now; }
        return false;
    }
};

struct ShowsSyncProgress {
    size_t completed=0, total=0;
    int percent(int previous=0) const {
        if (!total) return 100;
        const size_t bounded=std::min(completed,total);
        const int value=(int)((bounded*100)/total);
        return std::max(0,std::min(100,std::max(previous,value)));
    }
};

inline std::string librarySyncStatus(int tab, bool haveCache, bool offline,
                                     bool metadataActive, bool syncSucceeded,
                                     const ShowsSyncProgress &shows={}, bool hierarchyActive=false,
                                     bool showsTab=false) {
    if (tab < 0 || tab > 2) return "";
    if (offline && haveCache) return "OFFLINE";
    if ((showsTab || tab == 2) && shows.total && (hierarchyActive || shows.completed < shows.total))
        return "SYNC " + std::to_string(shows.percent()) + "%";
    if (metadataActive) return "SYNCING...";
    return syncSucceeded ? "SYNCED" : "SYNCING...";
}
} // namespace miyoofin

#endif // MIYOOFIN_HOME_SYNC_STATE_HPP
