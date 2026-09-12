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

void addContainer(std::map<std::string, MediaItem> &items,
                  const std::string &id, const std::string &type,
                  const std::string &title, const std::string &seriesId,
                  std::int32_t index)
{
    auto found = items.find(id);
    if (found == items.end()) {
        MediaItem item;
        item.id = id;
        item.type = type;
        item.title = title.empty() ? id : title;
        item.seriesId = seriesId;
        item.indexNumber = index;
        items.emplace(id, std::move(item));
    } else if (found->second.title > title && !title.empty()) {
        found->second.title = title;
    }
}
}

bool OfflineLibraryQuery::isAvailable(DownloadState state) {
    return state == DownloadState::Complete || state == DownloadState::LocalOnly
        || state == DownloadState::UpdateAvailable;
}

OfflineLibraryQuery::Hierarchy OfflineLibraryQuery::hierarchy(
    const std::vector<DownloadItem> &downloads)
{
    Hierarchy result;
    std::vector<DownloadItem> ordered = downloads;
    std::sort(ordered.begin(), ordered.end(),
              [](const DownloadItem &left, const DownloadItem &right) {
                  return left.itemId < right.itemId;
              });
    for (const auto &download : ordered) {
        if (!isAvailable(download.state) || download.itemId.empty()) continue;
        if (download.itemType == "movie") {
            MediaItem movie;
            movie.id = download.itemId;
            movie.type = "movie";
            movie.title = download.title.empty() ? download.itemId
                                                  : download.title;
            movie.runTimeTicks = download.runtimeTicks;
            movie.playbackPositionTicks = download.playbackPositionTicks;
            movie.progress = movie.runTimeTicks > 0
                ? static_cast<float>(movie.playbackPositionTicks)
                    / static_cast<float>(movie.runTimeTicks)
                : 0.0f;
            result.movies.push_back(std::move(movie));
            continue;
        }
        if (download.itemType != "episode"
            || download.seriesId.empty() || download.seasonId.empty()) {
            continue;
        }
        addContainer(result.series, download.seriesId, "show",
                     download.seriesName, {}, 0);
        addContainer(result.seasons, download.seasonId, "season",
                     download.seasonName, download.seriesId,
                     download.seasonNumber);
        MediaItem episode = fallback(download);
        episode.type = "episode";
        episode.progress = episode.runTimeTicks > 0
            ? static_cast<float>(episode.playbackPositionTicks)
                / static_cast<float>(episode.runTimeTicks)
            : 0.0f;
        result.episodesBySeason[download.seasonId].push_back(
            std::move(episode));
    }
    for (auto &entry : result.episodesBySeason) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const MediaItem &left, const MediaItem &right) {
                      if (left.indexNumber != right.indexNumber)
                          return left.indexNumber < right.indexNumber;
                      return left.id < right.id;
                  });
    }
    return result;
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
