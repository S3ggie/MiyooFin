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
}
#endif
