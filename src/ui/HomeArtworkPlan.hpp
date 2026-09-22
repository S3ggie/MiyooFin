#ifndef MIYOOFIN_HOME_ARTWORK_PLAN_HPP
#define MIYOOFIN_HOME_ARTWORK_PLAN_HPP

#include "../cache/LibraryCache.hpp"
#include "ArtworkLayout.hpp"
#include <string>
#include <vector>

namespace miyoofin {

struct HomePosterJob
{
    std::string itemId;
    ImageType imageType;
    std::string imageTag;
    int width;
    int height;
};

std::string homeArtworkKey(const MediaItem& item);
std::vector<HomePosterJob> planMediaPagePosterJobs(const std::vector<MediaItem>& items);
std::vector<HomePosterJob> planHomeRailPosterJobs(const std::vector<MediaItem>& continueWatching,
                                                  const std::vector<MediaItem>& recentlyAdded);
std::vector<HomePosterJob> planHomePosterJobs(const LibrarySnapshot& snapshot);
std::vector<HomePosterJob> planSeasonPosterJobs(const std::vector<MediaItem>& seasons);
/// Bounded, deduplicated series IDs extracted from continue-watching and
/// recently-added rail items (episodes carry a seriesId).  Used by the
/// season-poster prefetch to limit network requests to high-value series.
std::vector<std::string> collectBoundedSeriesIds(const std::vector<MediaItem>& continueWatching,
                                                 const std::vector<MediaItem>& recentlyAdded);

} // namespace miyoofin

#endif
