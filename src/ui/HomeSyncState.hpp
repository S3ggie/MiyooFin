#ifndef MIYOOFIN_HOME_SYNC_STATE_HPP
#define MIYOOFIN_HOME_SYNC_STATE_HPP

#include <SDL2/SDL.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
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

inline bool homeChangeNeedsPublication(bool catchUpRequired,
                                       bool catalogItemAffectsHome,
                                       bool cachedHomeItemRemoved)
{
    return catchUpRequired || catalogItemAffectsHome || cachedHomeItemRemoved;
}

/// Minimum interval between successive live-change rail refreshes.
/// Prevents the ~16-17 ResumeItems+LatestItems pairs per ~100s window
/// that each trigger a full network round-trip.
inline constexpr std::int64_t kHomeRailRefreshDebounceMs = 20 * 1000;

/// Returns true when a rail refresh should be skipped because the last
/// successful refresh completed within the debounce window.
/// Pure helper — testable without a HomeScreen instance.
inline bool homeRailRefreshDebounced(std::int64_t nowMs,
                                     std::int64_t lastSuccessMs,
                                     std::int64_t debounceMs = kHomeRailRefreshDebounceMs)
{
    return lastSuccessMs > 0 && (nowMs - lastSuccessMs) < debounceMs;
}

inline const char *homeSyncStatus(bool active)
{
    return active ? "HOME SYNCING..." : "";
}

inline std::string librarySyncStatus(int tab, bool haveCache, bool offline,
                                     bool metadataActive, bool syncSucceeded,
                                     const ShowsSyncProgress &shows={}, bool hierarchyActive=false,
                                     bool showsTab=false) {
    (void)showsTab;
    if (tab < 0 || tab > 2) return "";
    if (offline && haveCache) return "OFFLINE";
    if (shows.total && (metadataActive || hierarchyActive
                        || shows.completed < shows.total))
        return "SYNC " + std::to_string(shows.percent()) + "%";
    if (metadataActive) return "SYNCING...";
    return syncSucceeded ? "SYNCED" : "SYNCING...";
}

inline std::string artworkSyncStatus(bool active, bool planningComplete,
                                     const ShowsSyncProgress &artwork={}) {
    if (!active || !planningComplete || !artwork.total)
        return "";
    return "ART " + std::to_string(artwork.percent()) + "%";
}
} // namespace miyoofin

#endif // MIYOOFIN_HOME_SYNC_STATE_HPP
