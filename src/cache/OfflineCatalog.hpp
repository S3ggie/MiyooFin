#ifndef MIYOOFIN_OFFLINE_CATALOG_HPP
#define MIYOOFIN_OFFLINE_CATALOG_HPP

#include "../data/MediaItem.hpp"
#include <map>
#include <string>
#include <vector>

namespace miyoofin {
// A scoped navigation cache.  It deliberately contains metadata only; artwork
// remains owned by ImageCache and download bytes remain owned by DownloadStore.
struct OfflineCatalogSnapshot {
    std::map<std::string, MediaItem> series;
    std::map<std::string, std::vector<MediaItem> > seasonsBySeries;
    std::map<std::string, std::vector<MediaItem> > episodesBySeason;
};
class OfflineCatalog {
public:
    static std::string cachePath(const std::string &root, const std::string &scope);
    static bool save(const std::string &path, const OfflineCatalogSnapshot &snapshot, std::string *error=nullptr);
    static bool load(const std::string &path, OfflineCatalogSnapshot &snapshot, std::string *error=nullptr);
    static bool storeSeasons(const std::string &path, const MediaItem &series, const std::vector<MediaItem> &seasons, std::string *error=nullptr);
    static bool storeEpisodes(const std::string &path, const MediaItem &series, const MediaItem &season, const std::vector<MediaItem> &episodes, std::string *error=nullptr);
    static bool storeDiscoveredHierarchy(const std::string &path, const MediaItem &series,
                                         const std::vector<MediaItem> &seasons,
                                         const std::map<std::string, std::vector<MediaItem> > &episodesBySeason,
                                         bool complete=true, std::string *error=nullptr);
    static std::vector<MediaItem> seasons(const std::string &path, const std::string &seriesId);
    static std::vector<MediaItem> episodes(const std::string &path, const std::string &seasonId);
};
}
#endif
