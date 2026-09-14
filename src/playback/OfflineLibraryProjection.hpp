#ifndef MIYOOFIN_OFFLINE_LIBRARY_PROJECTION_HPP
#define MIYOOFIN_OFFLINE_LIBRARY_PROJECTION_HPP

#include "../cache/LibraryCache.hpp"
#include "../cache/OfflineCatalog.hpp"
#include "../download/DownloadTypes.hpp"
#include <map>
#include <set>

namespace miyoofin {
// UI projection: turns a LibrarySnapshot + offline catalog + DownloadManager
// snapshot into a playable main-thread view. This is the read-side consumer;
// OfflineLibraryQuery is the write-side builder that produces the snapshot.
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
