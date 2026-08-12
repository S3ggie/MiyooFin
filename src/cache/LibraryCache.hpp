#ifndef MIYOOFIN_LIBRARY_CACHE_HPP
#define MIYOOFIN_LIBRARY_CACHE_HPP

#include "../data/MediaItem.hpp"
#include <string>
#include <vector>

namespace miyoofin {
struct CachedLibraryView { std::string id, name, collectionType; std::vector<MediaItem> items; };
// Home rows are deliberately part of the same atomic snapshot as libraries:
// cached browsing must not wait for the network on application startup.
struct LibrarySnapshot {
    std::vector<CachedLibraryView> movies, shows;
    std::vector<MediaItem> continueWatching, recentlyAdded;
};
struct ReconcileStats { int added=0, changed=0, removed=0, unchanged=0, postersNeeded=0, stalePosters=0; };
struct StalePoster { std::string itemId, tag; };

class LibraryCache {
public:
    static std::string scopeKey(const std::string &serverUrl, const std::string &userId);
    static std::string cachePath(const std::string &root, const std::string &scope);
    static bool save(const std::string &path, const LibrarySnapshot &snapshot, std::string *error=nullptr);
    static bool load(const std::string &path, LibrarySnapshot &snapshot, std::string *error=nullptr,
                     bool *needsRefresh=nullptr);
    static ReconcileStats reconcile(const LibrarySnapshot &oldSnapshot, const LibrarySnapshot &remoteSnapshot,
                                    std::vector<StalePoster> *stale=nullptr);
    static bool itemEquivalent(const MediaItem &a, const MediaItem &b);
};
}
#endif
