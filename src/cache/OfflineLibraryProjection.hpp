#ifndef MIYOOFIN_OFFLINE_LIBRARY_PROJECTION_HPP
#define MIYOOFIN_OFFLINE_LIBRARY_PROJECTION_HPP

#include "LibraryCache.hpp"
#include "OfflineCatalog.hpp"
#include "../download/DownloadManager.hpp"
#include <map>
#include <set>

namespace miyoofin {
// A main-thread-safe view over the already reconciled DownloadManager snapshot.
// It intentionally never touches DownloadStore: filesystem validation happens
// while the manager loads/reconciles its index, not during UI updates/renders.
class OfflineLibraryProjection {
public:
    OfflineLibraryProjection(const LibrarySnapshot &library, const OfflineCatalogSnapshot &catalog,
                             const DownloadSnapshot &downloads);
    bool playable(const std::string &itemId) const;
    std::vector<MediaItem> movies() const;
    std::vector<MediaItem> series() const;
    std::vector<MediaItem> seasons(const std::string &seriesId) const;
    std::vector<MediaItem> episodes(const std::string &seasonId) const;
private:
    const LibrarySnapshot &m_library; const OfflineCatalogSnapshot &m_catalog;
    std::set<std::string> m_complete;
    std::map<std::string, MediaItem> m_media;
    std::map<std::string, MediaItem> m_series;
    std::map<std::string, MediaItem> m_seasons;
    std::map<std::string, std::vector<MediaItem> > m_episodes;
};
}
#endif
