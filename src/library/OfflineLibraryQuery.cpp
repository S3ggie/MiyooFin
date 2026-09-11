#include "OfflineLibraryQuery.hpp"
#include "../ui/MovieTitle.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace miyoofin {
namespace {
MediaItem fallback(const DownloadItem &download) {
    MediaItem item;
    item.id = download.itemId;
    item.type = download.itemType == "episode" ? "episode" :
                (download.itemType == "show" ? "show" : "movie");
    item.title = download.title.empty() ? download.itemId : download.title;
    item.seriesId = download.seriesId;
    item.seriesName = download.seriesName;
    item.seasonId = download.seasonId;
    item.indexNumber = download.episodeNumber;
    item.parentIndexNumber = download.seasonNumber;
    item.runTimeTicks = download.runtimeTicks;
    item.playbackPositionTicks = download.playbackPositionTicks;
    return item;
}
}

bool OfflineLibraryQuery::isAvailable(DownloadState state) {
    return state == DownloadState::Complete || state == DownloadState::LocalOnly
        || state == DownloadState::UpdateAvailable;
}

std::vector<std::vector<std::string>> OfflineLibraryQuery::metadataBatches(
    const DownloadSnapshot &downloads) {
    std::vector<std::string> ids;
    std::set<std::string> seen;
    for (const auto &download : downloads.items)
        if (isAvailable(download.state) && !download.itemId.empty()
            && seen.insert(download.itemId).second)
            ids.push_back(download.itemId);
    std::vector<std::vector<std::string>> batches;
    for (std::size_t offset = 0; offset < ids.size(); offset += kMetadataBatchSize)
        batches.emplace_back(ids.begin() + offset,
                             ids.begin() + std::min(ids.size(), offset + kMetadataBatchSize));
    return batches;
}

LibrarySnapshot OfflineLibraryQuery::build(const DownloadSnapshot &downloads,
                                           const std::vector<MediaItem> &metadata) {
    std::map<std::string, MediaItem> canonical;
    for (const auto &item : metadata) if (!item.id.empty()) canonical[item.id] = item;
    std::map<std::string, MediaItem> movies;
    std::map<std::string, MediaItem> shows;
    for (const auto &download : downloads.items) {
        if (!isAvailable(download.state) || download.itemId.empty()) continue;
        MediaItem item = canonical.count(download.itemId) ? canonical[download.itemId]
                                                           : fallback(download);
        if (item.type == "episode") {
            if (!item.seriesId.empty()) {
                auto found = canonical.find(item.seriesId);
                MediaItem series = found != canonical.end() ? found->second : fallback(download);
                series.id = item.seriesId; series.type = "show";
                if (series.title.empty()) series.title = download.seriesName.empty()
                    ? "Downloaded Show" : download.seriesName;
                shows[series.id] = series;
            }
        } else if (item.type == "movie") movies[item.id] = item;
    }
    LibrarySnapshot result;
    if (!movies.empty()) {
        CachedLibraryView view; view.id = "offline-movies"; view.name = "Movies";
        view.collectionType = "movies";
        for (const auto &entry : movies) view.items.push_back(entry.second);
        std::sort(view.items.begin(), view.items.end(), movieOrganizationalLess);
        result.movies.push_back(std::move(view));
    }
    if (!shows.empty()) {
        CachedLibraryView view; view.id = "offline-shows"; view.name = "Shows";
        view.collectionType = "tvshows";
        for (const auto &entry : shows) view.items.push_back(entry.second);
        std::sort(view.items.begin(), view.items.end(), movieOrganizationalLess);
        result.shows.push_back(std::move(view));
    }
    return result;
}
}
