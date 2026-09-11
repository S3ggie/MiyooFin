#ifndef MIYOOFIN_OFFLINE_LIBRARY_QUERY_HPP
#define MIYOOFIN_OFFLINE_LIBRARY_QUERY_HPP

#include "../data/MediaItem.hpp"
#include "../download/DownloadManager.hpp"
#include <vector>

namespace miyoofin {

// Builds the bounded offline root from the DownloadStore-derived snapshot.
// Catalog metadata can be supplied in batches by the caller; missing metadata
// is deliberately synthesized from DownloadItem and never affects availability.
class OfflineLibraryQuery {
public:
    static constexpr std::size_t kMetadataBatchSize = 64;

    static bool isAvailable(DownloadState state);
    static std::vector<std::vector<std::string>> metadataBatches(
        const DownloadSnapshot &downloads);
    static LibrarySnapshot build(const DownloadSnapshot &downloads,
                                 const std::vector<MediaItem> &metadata = {});
};

}
#endif
