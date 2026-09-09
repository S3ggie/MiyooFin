#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

bool catalogParityItemEqual(const MediaItem &expected,
                            const MediaItem &actual)
{
    const bool equal = expected.id == actual.id
        && expected.title == actual.title
        && expected.overview == actual.overview
        && expected.year == actual.year
        && expected.rating == actual.rating
        && expected.genre == actual.genre && expected.type == actual.type
        && expected.etag == actual.etag && expected.genres == actual.genres
        && expected.played == actual.played
        && expected.progress == actual.progress
        && expected.playbackPositionTicks == actual.playbackPositionTicks
        && expected.imageTags == actual.imageTags
        && expected.indexNumber == actual.indexNumber
        && expected.parentIndexNumber == actual.parentIndexNumber
        && expected.runTimeTicks == actual.runTimeTicks
        && expected.seriesName == actual.seriesName
        && expected.seriesId == actual.seriesId
        && expected.seasonId == actual.seasonId && expected.artR == actual.artR
        && expected.artG == actual.artG && expected.artB == actual.artB;
    if (!equal) {
        std::printf("[catalog parity] MediaItem mismatch for synthetic id %s\n",
                    expected.id.c_str());
    }
    return equal;
}

void checkCatalogParityItems(const std::vector<MediaItem> &legacy,
                             const std::vector<MediaItem> &sqlite)
{
    CHECK(legacy.size() == sqlite.size());
    const std::size_t count = std::min(legacy.size(), sqlite.size());
    for (std::size_t index = 0; index < count; ++index) {
        CHECK(legacy[index].id == sqlite[index].id);
        CHECK(catalogParityItemEqual(legacy[index], sqlite[index]));
    }
}

std::vector<std::string> catalogParityIds(const std::vector<MediaItem> &items)
{
    std::vector<std::string> ids;
    for (const auto &item : items) {
        ids.push_back(item.id);
    }
    return ids;
}

MediaItem catalogParityItem(const std::string &id, const std::string &type,
                            const std::string &title)
{
    MediaItem item;
    item.id = id;
    item.type = type;
    item.title = title;
    item.overview = "Parity overview " + id;
    item.year = 2026;
    item.rating = 7.25f;
    item.genre = "Drama";
    item.genres = {"Drama", "Science Fiction"};
    item.etag = "parity-etag-" + id;
    item.played = true;
    item.progress = 0.375f;
    item.playbackPositionTicks = 1234567;
    item.imageTags = {{"Backdrop", "backdrop-" + id},
                      {"Primary", "primary-" + id}};
    item.indexNumber = 1;
    item.parentIndexNumber = 2;
    item.runTimeTicks = 9876543;
    item.seriesName = "Parity 世界 Series";
    item.artR = 11;
    item.artG = 22;
    item.artB = 33;
    return item;
}

bool configureCatalogParityDb(CatalogDb &db, const std::string &url,
                              const std::string &user)
{
    const auto epoch = db.configureScope(url, user);
    if (!db.waitForIdleForTest(std::chrono::seconds(2))) {
        return false;
    }
    const auto state = db.scopeState();
    return state.requestedEpoch == epoch && state.ready;
}

void testCatalogDbLegacySqliteHierarchyParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-hierarchy-" + suffix
        + ".example";
    const std::string user = "parity-hierarchy-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series = catalogParityItem("__task16_series__", "show",
                                         "Parity 世界 Series");
    series.seriesName.clear();
    series.seriesId.clear();
    series.seasonId.clear();
    MediaItem zeroSeries = catalogParityItem("__task16_zero__", "show",
                                             "Zero Seasons");
    zeroSeries.genres = {"Documentary"};
    zeroSeries.genre = zeroSeries.genres.front();
    zeroSeries.seriesName.clear();
    zeroSeries.seriesId.clear();
    zeroSeries.seasonId.clear();

    MediaItem seasonAlpha = catalogParityItem("__task16_season_alpha__",
                                              "season", "Alpha");
    seasonAlpha.seriesId = series.id;
    seasonAlpha.seriesName = series.title;
    seasonAlpha.seasonId.clear();
    seasonAlpha.indexNumber = 1;
    MediaItem seasonBeta = seasonAlpha;
    seasonBeta.id = "__task16_season_beta__";
    seasonBeta.title = "Beta";
    seasonBeta.indexNumber = 1;

    MediaItem episodeAlpha = catalogParityItem("__task16_episode_alpha__",
                                               "episode", "Alpha Episode");
    episodeAlpha.seriesId = series.id;
    episodeAlpha.seasonId = seasonAlpha.id;
    episodeAlpha.seriesName = series.title;
    episodeAlpha.indexNumber = 1;
    episodeAlpha.parentIndexNumber = seasonAlpha.indexNumber;
    MediaItem episodeBeta = episodeAlpha;
    episodeBeta.id = "__task16_episode_beta__";
    episodeBeta.title = "Beta Episode";
    episodeBeta.indexNumber = 1;

    OfflineCatalogSnapshot legacy;
    legacy.series.emplace(series.id, series);
    legacy.series.emplace(zeroSeries.id, zeroSeries);
    legacy.seasonsBySeries[series.id] = {seasonAlpha, seasonBeta};
    legacy.episodesBySeason[seasonAlpha.id] = {episodeAlpha, episodeBeta};
    legacy.episodesBySeason[seasonBeta.id] = {};
    CHECK(OfflineCatalog::save(paths.legacyPath, legacy));

    const auto legacySeasons = OfflineCatalog::seasons(paths.legacyPath,
                                                       series.id);
    const auto legacyEpisodes = OfflineCatalog::episodes(
        paths.legacyPath, seasonAlpha.id);
    CHECK((catalogParityIds(legacySeasons)
           == std::vector<std::string>{seasonAlpha.id, seasonBeta.id}));
    CHECK((catalogParityIds(legacyEpisodes)
           == std::vector<std::string>{episodeAlpha.id, episodeBeta.id}));
    CHECK(OfflineCatalog::seasons(paths.legacyPath, zeroSeries.id).empty());

    // Phase A has no movie hierarchy population path.  The explicit empty
    // query is the compatibility assertion until a later roadmap task adds it.
    MediaItem movie = catalogParityItem("__task16_movie__", "movie",
                                        "Phase A Movie");
    LibrarySnapshot movieLibrary;
    movieLibrary.movies.push_back({"movies", "Movies", "movies", {movie}});
    DownloadSnapshot movieDownloads;
    DownloadItem movieDownload;
    movieDownload.itemId = movie.id;
    movieDownload.itemType = "movie";
    movieDownload.state = DownloadState::Complete;
    movieDownloads.items.push_back(movieDownload);
    OfflineCatalogSnapshot emptyCatalog;
    OfflineLibraryProjection movieProjection(movieLibrary, emptyCatalog,
                                              movieDownloads);
    CHECK(movieProjection.movies().size() == 1);
    if (movieProjection.movies().size() == 1) {
        CHECK(movieProjection.movies()[0].id == movie.id);
    }

    // CatalogDb opens a new final database after the legacy fixture is read,
    // so the old file is not accidentally used as a migration input here.
    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series, legacySeasons,
                                       legacy.episodesBySeason, 1, 100)
                  .get()
                  .success);
        CHECK(db.upsertSeriesHierarchy(zeroSeries, {}, {}, 2, 200)
                  .get()
                  .success);
        const auto sqliteSeasons = db.getSeasons(series.id).get();
        const auto sqliteEpisodes = db.getEpisodes(seasonAlpha.id).get();
        CHECK(sqliteSeasons.success && sqliteEpisodes.success);
        checkCatalogParityItems(legacySeasons, sqliteSeasons.items);
        checkCatalogParityItems(legacyEpisodes, sqliteEpisodes.items);
        CHECK(db.getSeasons(zeroSeries.id).get().items.empty());
        CHECK(db.getSeasons(movie.id).get().items.empty());
    }
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbProjectionParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-projection-" + suffix
        + ".example";
    const std::string user = "parity-projection-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series = catalogParityItem("__task16_projection_series__",
                                         "show", "Projected Series");
    series.seriesName.clear();
    series.seriesId.clear();
    series.seasonId.clear();
    MediaItem season = catalogParityItem("__task16_projection_season__",
                                         "season", "Projected Season");
    season.seriesId = series.id;
    season.seriesName = series.title;
    season.seasonId.clear();
    MediaItem completeEpisode = catalogParityItem(
        "__task16_projection_complete__", "episode", "Complete Episode");
    completeEpisode.seriesId = series.id;
    completeEpisode.seasonId = season.id;
    completeEpisode.seriesName = series.title;
    completeEpisode.parentIndexNumber = season.indexNumber;
    MediaItem unavailableEpisode = completeEpisode;
    unavailableEpisode.id = "__task16_projection_unavailable__";
    unavailableEpisode.title = "Unavailable Episode";
    unavailableEpisode.indexNumber = 2;

    OfflineCatalogSnapshot catalog;
    catalog.series[series.id] = series;
    catalog.seasonsBySeries[series.id] = {season};
    catalog.episodesBySeason[season.id] = {completeEpisode,
                                            unavailableEpisode};
    LibrarySnapshot library;
    library.shows.push_back({"shows", "Shows", "tvshows", {series}});
    DownloadSnapshot downloads;
    DownloadItem complete;
    complete.itemId = completeEpisode.id;
    complete.itemType = "episode";
    complete.state = DownloadState::Complete;
    complete.seriesId = series.id;
    complete.seasonId = season.id;
    complete.episodeNumber = completeEpisode.indexNumber;
    complete.seasonNumber = season.indexNumber;
    downloads.items.push_back(complete);

    OfflineLibraryProjection projection(library, catalog, downloads);
    const auto projectedSeasons = projection.seasons(series.id);
    const auto projectedEpisodes = projection.episodes(season.id);
    CHECK(projectedSeasons.size() == 1);
    CHECK(projectedEpisodes.size() == 1);
    if (projectedEpisodes.size() == 1) {
        CHECK(catalogParityItemEqual(completeEpisode, projectedEpisodes[0]));
    }

    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series, {season},
                                       {{season.id, {completeEpisode}}}, 1, 100)
                  .get()
                  .success);
        const auto sqliteSeasons = db.getSeasons(series.id).get();
        const auto sqliteEpisodes = db.getEpisodes(season.id).get();
        CHECK(sqliteSeasons.success && sqliteEpisodes.success);
        checkCatalogParityItems(projectedSeasons, sqliteSeasons.items);
        checkCatalogParityItems(projectedEpisodes, sqliteEpisodes.items);
    }
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbDownloadFallbackParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-fallback-" + suffix
        + ".example";
    const std::string user = "parity-fallback-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    DownloadSnapshot downloads;
    DownloadItem downloaded;
    downloaded.itemId = "__task16_fallback_episode__";
    downloaded.itemType = "episode";
    downloaded.title = "Fallback Episode 世界";
    downloaded.seriesId = "__task16_fallback_series__";
    downloaded.seriesName = "Fallback Series";
    downloaded.seasonId = "__task16_fallback_season__";
    downloaded.seasonName = "Fallback Season";
    downloaded.episodeNumber = 4;
    downloaded.seasonNumber = 2;
    downloaded.playbackPositionTicks = 456789;
    downloaded.runtimeTicks = 9876543;
    downloaded.state = DownloadState::Complete;
    downloads.items.push_back(downloaded);
    LibrarySnapshot emptyLibrary;
    OfflineCatalogSnapshot missingCatalog;
    OfflineLibraryProjection projection(emptyLibrary, missingCatalog,
                                        downloads);
    const auto projectedSeries = projection.series();
    const auto projectedSeasons = projection.seasons(downloaded.seriesId);
    const auto projectedEpisodes = projection.episodes(downloaded.seasonId);
    CHECK(projectedSeries.size() == 1 && projectedSeasons.size() == 1
          && projectedEpisodes.size() == 1);
    MediaItem fallbackEpisode;
    fallbackEpisode.id = downloaded.itemId;
    fallbackEpisode.type = "episode";
    fallbackEpisode.title = downloaded.title;
    fallbackEpisode.seriesId = downloaded.seriesId;
    fallbackEpisode.seriesName = downloaded.seriesName;
    fallbackEpisode.seasonId = downloaded.seasonId;
    fallbackEpisode.indexNumber = downloaded.episodeNumber;
    fallbackEpisode.parentIndexNumber = downloaded.seasonNumber;
    fallbackEpisode.playbackPositionTicks = downloaded.playbackPositionTicks;
    fallbackEpisode.runTimeTicks = downloaded.runtimeTicks;
    if (projectedEpisodes.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackEpisode, projectedEpisodes[0]));
    }
    MediaItem fallbackSeries;
    fallbackSeries.id = downloaded.seriesId;
    fallbackSeries.type = "show";
    fallbackSeries.title = downloaded.seriesName;
    MediaItem fallbackSeason;
    fallbackSeason.id = downloaded.seasonId;
    fallbackSeason.type = "season";
    fallbackSeason.title = downloaded.seasonName;
    fallbackSeason.seriesId = downloaded.seriesId;
    fallbackSeason.indexNumber = downloaded.seasonNumber;
    if (projectedSeries.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackSeries, projectedSeries[0]));
    }
    if (projectedSeasons.size() == 1) {
        CHECK(catalogParityItemEqual(fallbackSeason, projectedSeasons[0]));
    }

    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(fallbackSeries, {fallbackSeason},
                                       {{fallbackSeason.id, {fallbackEpisode}}},
                                       1, 100)
                  .get()
                  .success);
        checkCatalogParityItems(projectedSeasons,
                                db.getSeasons(fallbackSeries.id).get().items);
        checkCatalogParityItems(projectedEpisodes,
                                db.getEpisodes(fallbackSeason.id).get().items);
    }
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbDuplicateMergeAndAuthoritativeDeletionParity()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-parity-reconcile-" + suffix
        + ".example";
    const std::string user = "parity-reconcile-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series = catalogParityItem("__task16_keep_series__", "show",
                                         "Keep Series");
    series.seriesName.clear();
    series.seriesId.clear();
    series.seasonId.clear();
    MediaItem season = catalogParityItem("__task16_keep_season__", "season",
                                         "Original Season");
    season.seriesId = series.id;
    season.seriesName = series.title;
    season.seasonId.clear();
    MediaItem episode = catalogParityItem("__task16_keep_episode__",
                                          "episode", "Original Episode");
    episode.seriesId = series.id;
    episode.seriesName = series.title;
    episode.seasonId = season.id;
    episode.parentIndexNumber = season.indexNumber;

    CHECK(OfflineCatalog::save(paths.legacyPath, OfflineCatalogSnapshot{}));
    MediaItem mergedSeason = season;
    mergedSeason.title = "Merged Season";
    MediaItem mergedEpisode = episode;
    mergedEpisode.title = "Merged Episode";
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(
        paths.legacyPath, series, {season}, {{season.id, {episode}}}));
    CHECK(OfflineCatalog::storeDiscoveredHierarchy(
        paths.legacyPath, series, {mergedSeason},
        {{season.id, {mergedEpisode, mergedEpisode}}}));
    OfflineCatalogSnapshot mergedLegacy;
    CHECK(OfflineCatalog::load(paths.legacyPath, mergedLegacy));
    CHECK(mergedLegacy.seasonsBySeries[series.id].size() == 1);
    CHECK(mergedLegacy.episodesBySeason[season.id].size() == 1);
    CHECK(mergedLegacy.seasonsBySeries[series.id][0].title == "Merged Season");
    CHECK(mergedLegacy.episodesBySeason[season.id][0].title
          == "Merged Episode");

    MediaItem removedSeries = series;
    removedSeries.id = "__task16_removed_series__";
    removedSeries.title = "Removed Series";
    MediaItem removedSeason = season;
    removedSeason.id = "__task16_removed_season__";
    removedSeason.seriesId = removedSeries.id;
    MediaItem removedEpisode = episode;
    removedEpisode.id = "__task16_removed_episode__";
    removedEpisode.seriesId = removedSeries.id;
    removedEpisode.seasonId = removedSeason.id;
    mergedLegacy.series[removedSeries.id] = removedSeries;
    mergedLegacy.seasonsBySeries[removedSeries.id] = {removedSeason};
    mergedLegacy.episodesBySeason[removedSeason.id] = {removedEpisode};
    CHECK(OfflineCatalog::save(paths.legacyPath, mergedLegacy));
    CHECK(OfflineCatalog::reconcileSeries(paths.legacyPath, {series}));
    OfflineCatalogSnapshot legacyAfter = {};
    CHECK(OfflineCatalog::load(paths.legacyPath, legacyAfter));
    CHECK(legacyAfter.series.count(series.id) == 1);
    CHECK(legacyAfter.series.count(removedSeries.id) == 0);
    CHECK(legacyAfter.seasonsBySeries.count(removedSeries.id) == 0);
    CHECK(legacyAfter.episodesBySeason.count(removedSeason.id) == 0);

    std::remove(paths.legacyPath.c_str());
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series,
                                       legacyAfter.seasonsBySeries[series.id],
                                       legacyAfter.episodesBySeason, 1, 100)
                  .get()
                  .success);
        CHECK(db.upsertSeriesHierarchy(removedSeries, {removedSeason},
                                       {{removedSeason.id, {removedEpisode}}},
                                       2, 200)
                  .get()
                  .success);
        const auto sqliteBefore = db.reconcileSeries({series}, true).get();
        CHECK(sqliteBefore.success && sqliteBefore.authoritative
              && sqliteBefore.seriesDeleted == 1);
        CHECK(db.getSeasons(removedSeries.id).get().items.empty());
        const auto sqliteKeepSeasons = db.getSeasons(series.id).get();
        const auto sqliteKeepEpisodes = db.getEpisodes(season.id).get();
        CHECK(sqliteKeepSeasons.success && sqliteKeepEpisodes.success);
        checkCatalogParityItems(legacyAfter.seasonsBySeries[series.id],
                                sqliteKeepSeasons.items);
        checkCatalogParityItems(legacyAfter.episodesBySeason[season.id],
                                sqliteKeepEpisodes.items);
    }
    removeCatalogMigrationTestPaths(paths);
}

} // namespace
