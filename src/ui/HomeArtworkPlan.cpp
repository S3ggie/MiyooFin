#include "HomeArtworkPlan.hpp"
#include "../data/TitleOrganization.hpp"
#include <set>
#include <algorithm>

namespace miyoofin {

std::string homeArtworkKey(const MediaItem &item)
{
    return buildRowArtworkKey(item);
}

std::vector<HomePosterJob> planMediaPagePosterJobs(const std::vector<MediaItem> &items)
{
    std::vector<HomePosterJob> out;
    std::set<std::string> seen;
    std::vector<const MediaItem *> ordered;
    ordered.reserve(items.size());
    for (const auto &item : items)
        ordered.push_back(&item);
    std::sort(ordered.begin(), ordered.end(), [](const MediaItem *left,
                                                 const MediaItem *right) {
        return organizationalLess(*left, *right);
    });
    for (const MediaItem *item : ordered) {
        DisplayArtwork artwork=displayArtworkForItem(*item);
        if (!artwork.valid()) continue;
        const std::string key=homeArtworkKey(*item);
        if (!seen.insert(key).second) continue;
        out.push_back({item->id,artwork.imageType,artwork.tag,artwork.width,artwork.height});
    }
    return out;
}

std::vector<HomePosterJob> planHomeRailPosterJobs(
    const std::vector<MediaItem> &continueWatching,
    const std::vector<MediaItem> &recentlyAdded)
{
    std::vector<HomePosterJob> out;
    std::set<std::string> seen;
    const auto add = [&](const MediaItem &item) {
        const DisplayArtwork artwork = displayArtworkForItem(item);
        if (!artwork.valid()) return;
        const std::string key = homeArtworkKey(item);
        if (!seen.insert(key).second) return;
        out.push_back({item.id, artwork.imageType, artwork.tag,
                       artwork.width, artwork.height});
    };
    for (const auto &item : continueWatching) add(item);
    for (const auto &item : recentlyAdded) add(item);
    return out;
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
