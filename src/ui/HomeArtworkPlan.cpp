#include "HomeArtworkPlan.hpp"
#include <set>

namespace miyoofin {

std::string homeArtworkKey(const MediaItem &item)
{
    return buildRowArtworkKey(item);
}

std::vector<HomePosterJob> planHomePosterJobs(const LibrarySnapshot &snapshot)
{
    std::vector<HomePosterJob> out; std::set<std::string> seen;
    auto add=[&](const MediaItem &item) { DisplayArtwork a=displayArtworkForItem(item); if(!a.valid()) return; HomePosterJob j{item.id,a.imageType,a.tag,a.width,a.height}; std::string key=homeArtworkKey(item); if(seen.insert(key).second) out.push_back(std::move(j)); };
    for(const auto &views : {&snapshot.movies,&snapshot.shows}) for(const auto &view:*views) for(const auto &item:view.items) add(item);
    for(const auto &item:snapshot.continueWatching) add(item);
    for(const auto &item:snapshot.recentlyAdded) add(item);
    return out;
}

std::vector<HomePosterJob> planSeasonPosterJobs(const std::vector<MediaItem> &seasons)
{
    std::vector<HomePosterJob> out; std::set<std::string> seen;
    for (const auto &season : seasons) {
        if (!season.type.empty() && season.type != "season") continue;
        auto tag=season.imageTags.find("Primary");
        if (season.id.empty() || tag==season.imageTags.end() || tag->second.empty()) continue;
        HomePosterJob job{season.id,ImageType::Primary,tag->second,74,111};
        std::string key=job.itemId+":"+job.imageTag;
        if (seen.insert(key).second) out.push_back(std::move(job));
    }
    return out;
}

} // namespace miyoofin
