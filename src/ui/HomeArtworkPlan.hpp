#ifndef MIYOOFIN_HOME_ARTWORK_PLAN_HPP
#define MIYOOFIN_HOME_ARTWORK_PLAN_HPP

#include "../cache/LibraryCache.hpp"
#include "ArtworkLayout.hpp"
#include <string>
#include <vector>

namespace miyoofin {

struct HomePosterJob {
    std::string itemId;
    ImageType imageType;
    std::string imageTag;
    int width;
    int height;
};

std::string homeArtworkKey(const MediaItem &item);
std::vector<HomePosterJob> planHomePosterJobs(const LibrarySnapshot &snapshot);
std::vector<HomePosterJob> planSeasonPosterJobs(const std::vector<MediaItem> &seasons);

} // namespace miyoofin

#endif
