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

void testCatalogDbLibrarySnapshotSeed()
{
    const std::string url = "https://sqlite-library-seed.example";
    const std::string user = "library-seed-user";
    auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    LibrarySnapshot snapshot;
    MediaItem movie = catalogParityItem("__task29_movie__", "movie", "Seed Movie");
    MediaItem secondMovie = catalogParityItem("__task29_movie_2__", "movie", "Seed Movie 2");
    snapshot.movies.push_back({"movies", "Movies", "movies", {movie}});
    snapshot.movies[0].items.push_back(secondMovie);
    snapshot.continueWatching.push_back(movie);
    auto first = db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get();
    CHECK(first.success && first.workerOwned && first.itemsUpserted == 2
          && first.viewsWritten == 1 && first.homeItemsWritten == 1);
    auto read = db.readLibrarySnapshot({0, epoch, {}}).get();
    CHECK(read.success && read.snapshot.movies.size() == 1
          && read.snapshot.movies[0].collectionType == "movies"
          && read.snapshot.continueWatching.size() == 1
          && read.snapshot.continueWatching[0].id == movie.id);
    auto second = db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get();
    CHECK(second.success && second.itemsUpserted == 2 && second.viewsWritten == 1);
    auto failed = db.seedLibrarySnapshotForTest(snapshot, 1, {0, epoch, {}}).get();
    CHECK(!failed.success && failed.error == CatalogDbErrorCategory::SqliteError);
    auto recovered = db.seedLibrarySnapshot(LibrarySnapshot{}, {0, epoch, {}}).get();
    CHECK(recovered.success && recovered.itemsUpserted == 0);
    auto nextEpoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto stale = db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get();
    CHECK(!stale.success && stale.superseded);
    auto empty = db.seedLibrarySnapshot(LibrarySnapshot{}, {0, nextEpoch, {}}).get();
    CHECK(empty.success && empty.itemsUpserted == 0 && empty.viewsWritten == 0);
    removeCatalogMigrationTestPaths(paths);
}

void testCatalogDbTopLevelSyncStagingLifecycle()
{
    const std::string url = "https://sqlite-top-level-stage.example";
    const std::string user = "top-level-stage-user";
    auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    LibrarySnapshot snapshot;
    snapshot.movies.push_back({"movies", "Movies", "movies",
                                {catalogParityItem("__stage_movie__", "movie", "Stage Movie")} });
    CHECK(db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get().success);
    auto before = db.readLibrarySnapshot({0, epoch, {}}).get();
    auto begin = db.beginTopLevelSync(101, {0, epoch, {}}).get();
    CHECK(begin.success && begin.workerOwned && begin.generation == 101);
    auto newer = db.beginTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(newer.success && newer.generation == 102);
    auto abort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(abort.success && abort.workerOwned);
    auto repeatAbort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(repeatAbort.success);
    auto after = db.readLibrarySnapshot({0, epoch, {}}).get();
    CHECK(before.success && after.success && after.snapshot.movies.size() == 1
          && after.snapshot.movies[0].items.size() == 1
          && after.snapshot.movies[0].items[0].id == "__stage_movie__");
    CHECK(db.beginTopLevelSync(104, {0, epoch, {}}).get().success);
    CatalogDbMediaPageWrite replacement;
    replacement.items = {catalogParityItem("__replacement_movie__", "movie", "Replacement Movie")};
    replacement.viewId = "movies"; replacement.viewName = "Movies";
    replacement.collectionType = "movies"; replacement.syncGeneration = 104;
    CHECK(db.upsertMediaPage(replacement, {0, epoch, {}}).get().success);
    auto finalized = db.finalizeTopLevelSync(104, {0, epoch, {}}).get();
    CHECK(finalized.success && finalized.workerOwned);
    auto published = db.readLibrarySnapshot({0, epoch, {}}).get();
    CHECK(published.success && published.snapshot.movies.size() == 1
          && published.snapshot.movies[0].items.size() == 1
          && published.snapshot.movies[0].items[0].id == "__replacement_movie__");
    const auto mediaPage = db.readMediaPage(
        "movie", -1, 8, {}, {0, epoch, {}}).get();
    CHECK(mediaPage.success && mediaPage.items.size() == 1
          && mediaPage.items[0].id == "__replacement_movie__");
    db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto stale = db.beginTopLevelSync(103, {0, epoch, {}}).get();
    CHECK(!stale.success && stale.error == CatalogDbErrorCategory::ScopeNotReady);
    removeCatalogMigrationTestPaths(paths);
}

static std::vector<std::string> collectCatalogPageIds(
    CatalogDb &db, const std::string &type, int letter, std::size_t pageSize,
    std::uint64_t epoch)
{
    std::vector<std::string> ids;
    CatalogDbPageCursor cursor;
    for (std::size_t page = 0; page < 4096; ++page) {
        const auto result = db.readMediaPage(
            type, letter, pageSize, cursor, {0, epoch, {}}).get();
        CHECK(result.success && result.workerOwned && !result.cancelled
              && !result.superseded);
        std::set<std::string> pageIds;
        for (const auto &item : result.items) {
            CHECK(pageIds.insert(item.id).second);
            CHECK(std::find(ids.begin(), ids.end(), item.id) == ids.end());
            ids.push_back(item.id);
        }
        if (!result.hasMore) {
            CHECK(!result.items.empty() || ids.empty());
            break;
        }
        CHECK(result.next.valid);
        cursor = result.next;
    }
    return ids;
}

static void testCatalogDbBoundedMediaPaging()
{
    std::printf("[test] CatalogDb bounded media paging\n");
    const std::string url = "https://sqlite-media-pages.example";
    const std::string user = "media-pages-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    std::vector<MediaItem> movies;
    const std::vector<std::string> titles = {
        "The apple", "APPLE", "Apple", "The APPLE", "apple",
        "The apple pie", "apple pie", "A", "a", "A!", "A-",
        "The", "", "Same", "Same", "Same", "Zed", "zebra",
        "Ärger", "世界", "éclair", "The 2", "2 Fast", "_hidden",
    };
    for (std::size_t i = 0; i < titles.size(); ++i) {
        movies.push_back(catalogParityItem(
            "__task32_movie_" + std::to_string(i) + "__", "movie",
            titles[i]));
    }
    LibrarySnapshot snapshot;
    snapshot.movies.push_back({"movies", "Movies", "movies", movies});
    MediaItem show = catalogParityItem("__task32_show__", "show", "The Show");
    snapshot.shows.push_back({"shows", "Shows", "tvshows", {show}});

    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get().success);

    auto expected = movies;
    std::sort(expected.begin(), expected.end(), organizationalLess);
    std::vector<std::string> expectedIds;
    for (const auto &item : expected) expectedIds.push_back(item.id);
    for (const std::size_t pageSize : {std::size_t(1), std::size_t(2),
                                       std::size_t(3), std::size_t(7),
                                       std::size_t(24)}) {
        CHECK(collectCatalogPageIds(db, "movie", -1, pageSize, epoch)
              == expectedIds);
    }
    const auto repeated = collectCatalogPageIds(db, "movie", -1, 3, epoch);
    CHECK(repeated == collectCatalogPageIds(db, "movie", -1, 3, epoch));

    for (int letter = 0; letter < 26; ++letter) {
        std::vector<std::string> expectedLetter;
        for (const auto &item : expected)
            if (matchesAlphabetFilter(item.title, letter))
                expectedLetter.push_back(item.id);
        CHECK(collectCatalogPageIds(db, "movie", letter, 1, epoch)
              == expectedLetter);
    }

    snapshot.movies[0].items[0].title = "Aardvark";
    CHECK(db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get().success);
    expected = snapshot.movies[0].items;
    std::sort(expected.begin(), expected.end(), organizationalLess);
    expectedIds.clear();
    for (const auto &item : expected) expectedIds.push_back(item.id);
    CHECK(collectCatalogPageIds(db, "movie", -1, 2, epoch) == expectedIds);

    const auto plan = db.runMediaPageQueryPlanForTest("movie", -1);
    CHECK(plan.success && plan.workerOwned && plan.mediaPageUsesSortIndex
          && plan.mediaPageAvoidsTempSort);

    CatalogDb empty;
    const auto emptyEpoch = empty.configureScope(
        "https://sqlite-media-pages-empty.example", "empty-user");
    CHECK(empty.waitForIdleForTest(std::chrono::seconds(2)));
    const auto emptyPage = empty.readMediaPage(
        "movie", -1, 1, {}, {0, emptyEpoch, {}}).get();
    CHECK(emptyPage.success && emptyPage.items.empty() && !emptyPage.hasMore);

    CatalogDb staleGeneration;
    const auto staleEpoch = staleGeneration.configureScope(
        "https://sqlite-media-pages-stale-generation.example", "stale-user");
    CHECK(staleGeneration.waitForIdleForTest(std::chrono::seconds(2)));
    staleGeneration.setGenerationForTest(1);
    staleGeneration.setWorkerPausedForTest(true);
    auto staleFuture = staleGeneration.readMediaPage(
        "movie", -1, 1, {}, {1, staleEpoch, {}});
    staleGeneration.setGenerationForTest(2);
    staleGeneration.setWorkerPausedForTest(false);
    CHECK(staleGeneration.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(staleFuture.get().superseded);

    CatalogDb staleScope;
    const auto firstEpoch = staleScope.configureScope(
        "https://sqlite-media-pages-stale-scope-a.example", "scope-user");
    CHECK(staleScope.waitForIdleForTest(std::chrono::seconds(2)));
    staleScope.setWorkerPausedForTest(true);
    auto staleScopeFuture = staleScope.readMediaPage(
        "movie", -1, 1, {}, {0, firstEpoch, {}});
    const auto secondEpoch = staleScope.configureScope(
        "https://sqlite-media-pages-stale-scope-b.example", "scope-user");
    staleScope.setWorkerPausedForTest(false);
    CHECK(staleScope.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(staleScope.scopeState().requestedEpoch == secondEpoch);
    CHECK(staleScopeFuture.get().superseded);

    const std::string homeSource = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(homeSource.find("readMediaPage") != std::string::npos);
    CHECK(homeSource.find("std::thread([this") != std::string::npos);
    CHECK(homeSource.find("getLibraryItemsPage") != std::string::npos);
    CHECK(homeSource.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(homeSource.find("readLibrarySnapshot") == std::string::npos);
    CHECK(homeSource.find("home_items") == std::string::npos);
    const std::string refreshSource = readTestBytes("src/ui/screens/HomeScreenRefresh.cpp");
    CHECK(refreshSource.find("home_items") == std::string::npos);
    CHECK(homeSource.find("m_movieMaster=combineMovieViews") == std::string::npos);
    CHECK(homeSource.find("m_showMaster=combineShowViews") == std::string::npos);
    CHECK(homeSource.find("rebuildShowsPresentation();if(m_session.manualOfflineMode)") == std::string::npos);
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] CatalogDb bounded media paging OK\n");
}

static void testCatalogDbMediaPagePreservesViewMembership()
{
    std::printf("[test] CatalogDb media pages preserve TV view membership\n");
    const std::string url = "https://sqlite-media-membership.example";
    const std::string user = "media-membership-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem anime = catalogParityItem("__task33_anime_series__", "show",
                                        "Anime Series");
    anime.genres = {"Animation"};
    LibrarySnapshot snapshot;
    snapshot.shows = {
        {"shows", "Shows", "tvshows", {anime}},
        {"anime-view", "Anime", "tvshows", {anime}},
    };

    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.seedLibrarySnapshot(snapshot, {0, epoch, {}}).get().success);

    const auto page = db.readMediaPage(
        "show", -1, 8, {}, {0, epoch, {}}).get();
    CHECK(page.success && page.items.size() == 1);
    const auto memberships = page.membershipsByItem.find(anime.id);
    CHECK(memberships != page.membershipsByItem.end());
    if (memberships != page.membershipsByItem.end()) {
        CHECK(memberships->second.size() == 2);
        const bool hasAnimeView = std::any_of(
            memberships->second.begin(), memberships->second.end(),
            [](const CatalogDbMediaPageMembership &membership) {
                return membership.viewName == "Anime"
                    && membership.collectionType == "tvshows";
            });
        CHECK(hasAnimeView);
    }
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] CatalogDb media pages preserve TV view membership OK\n");
}

void testLibraryCacheHomeParityHarness()
{
    LibrarySnapshot snapshot;
    MediaItem a = catalogParityItem("__task30_a__", "movie", "The apple");
    MediaItem b = catalogParityItem("__task30_b__", "movie", "APPLE");
    MediaItem c = catalogParityItem("__task30_c__", "movie", "Apple");
    snapshot.movies = {{"v1", "Primary Movies", "movies", {a, b}},
                       {"v2", "Secondary Movies", "movies", {c}}};
    snapshot.shows = {{"shows", "Shows", "tvshows", {a}}};
    snapshot.continueWatching = {c, a};
    snapshot.recentlyAdded = {b, c};
    const auto tabs = tabsFromSnapshot(snapshot);
    CHECK(tabs.size() == 5 && tabs[0].rows.size() == 2);
    CHECK(tabs[0].rows[0].label == "Continue Watching"
          && tabs[0].rows[0].items[0].id == c.id);
    CHECK(tabs[0].rows[1].label == "Recently Added"
          && tabs[0].rows[1].items[1].id == c.id);
    CHECK(tabs[1].rows[0].items.size() == 3);
    CHECK(tabs[1].rows[0].items[0].id == b.id
          && tabs[1].rows[0].items[1].id == c.id
          && tabs[1].rows[0].items[2].id == a.id);
    CHECK(tabs[2].rows.size() == 1 && tabs[2].rows[0].label == "Shows"
          && tabs[2].rows[0].items[0].id == a.id);
    CHECK(offlineTabsFromSnapshot(snapshot).size() == 4);
}

static void testCatalogDbMediaPageUpsertAndPopulation()
{
    std::printf("[test] CatalogDb page upsert atomicity and population states\n");
    CatalogDb db;
    const auto epoch = db.configureScope("https://page-upsert.example", "user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const CatalogDbJobMetadata metadata{0, epoch, {}};
    auto item = catalogParityItem("page-item", "movie", "The Page Item");
    CatalogDbMediaPageWrite first;
    first.items = {item}; first.viewId = "movies"; first.viewName = "Movies";
    first.collectionType = "movies"; first.ordinalStart = 4; first.finalPage = false;
    auto written = db.upsertMediaPage(first, metadata).get();
    CHECK(written.success && written.workerOwned && written.rowsWritten == 1);
    CHECK(db.populationStatus().state == CatalogDbPopulationState::Populating);
    auto again = db.upsertMediaPage(first, metadata).get();
    CHECK(again.success && again.rowsWritten == 1);
    item.title = "Updated Page Item";
    CatalogDbMediaPageWrite final = first; final.items = {item}; final.finalPage = true;
    CHECK(db.upsertMediaPage(final, metadata).get().success);
    CHECK(db.populationStatus().state == CatalogDbPopulationState::Ready);
    auto snapshot = db.readLibrarySnapshot(metadata).get();
    CHECK(snapshot.success && snapshot.snapshot.movies.size() == 1
          && snapshot.snapshot.movies[0].items.size() == 1
          && snapshot.snapshot.movies[0].items[0].title == "Updated Page Item");
    CatalogDbMediaPageWrite failing = first;
    failing.items = {catalogParityItem("rollback-a", "movie", "A"),
                     catalogParityItem("rollback-b", "movie", "B")};
    auto failed = db.upsertMediaPageForTest(failing, 1, metadata).get();
    CHECK(!failed.success && failed.error == CatalogDbErrorCategory::SqliteError);
    auto after = db.readMediaPage("movie", -1, 64, {}, metadata).get();
    CHECK(after.success && std::none_of(after.items.begin(), after.items.end(),
        [](const MediaItem &v) { return v.id == "rollback-a" || v.id == "rollback-b"; }));
    auto cancel = std::make_shared<std::atomic_bool>(true);
    auto cancelled = db.upsertMediaPage(final, {0, epoch, cancel}).get();
    CHECK(cancelled.cancelled && !cancelled.success);
    std::printf("[test] CatalogDb page upsert atomicity and population states OK\n");
}

static void testCatalogDbMediaPageTopLevelStaging()
{
    std::printf("[test] CatalogDb staged media page writes\n");
    CatalogDb db;
    const auto epoch = db.configureScope("https://page-stage.example", "user");
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const CatalogDbJobMetadata metadata{0, epoch, {}};
    CatalogDbMediaPageWrite live;
    live.items = {catalogParityItem("live-stage", "movie", "Live Stage")};
    live.viewId = "movies"; live.viewName = "Movies"; live.collectionType = "movies";
    CHECK(db.upsertMediaPage(live, metadata).get().success);
    CHECK(db.beginTopLevelSync(77, metadata).get().success);
    CatalogDbMediaPageWrite staged = live;
    staged.items = {catalogParityItem("staged-a", "movie", "A"),
                    catalogParityItem("staged-b", "movie", "B")};
    staged.syncGeneration = 77; staged.ordinalStart = 4;
    CHECK(db.upsertMediaPage(staged, metadata).get().success);
    auto before = db.readLibrarySnapshot(metadata).get();
    CHECK(before.success && before.snapshot.movies.size() == 1
          && before.snapshot.movies[0].items[0].id == "live-stage");
    auto stale = staged; stale.syncGeneration = 76;
    CHECK(db.upsertMediaPage(stale, metadata).get().superseded);
    auto failed = staged; failed.items = {catalogParityItem("rollback-stage", "movie", "R")};
    CHECK(!db.upsertMediaPageForTest(failed, 0, metadata).get().success);
    CHECK(db.abortTopLevelSync(77, metadata).get().success);
    auto after = db.readLibrarySnapshot(metadata).get();
    CHECK(after.success && after.snapshot.movies[0].items[0].id == "live-stage");
    std::printf("[test] CatalogDb staged media page writes OK\n");
}

static void testHomeSkipsUnsupportedLibraryViews()
{
    std::printf("[test] Home skips unsupported library views before persistence\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("supportedLibraryView") != std::string::npos);
    CHECK(source.find("view.collectionType == \"movies\"") != std::string::npos);
    CHECK(source.find("view.collectionType == \"tvshows\"") != std::string::npos);
    CHECK(source.find("library_view_skipped unsupported_collection") != std::string::npos);
    CHECK(source.find("views.erase(std::remove_if") != std::string::npos);
    CHECK(source.find("getLibraryItemsPage") != std::string::npos);
    CHECK(source.find("m_librarySync->stage") != std::string::npos);
    CHECK(source.find("beginTopLevelSync") == std::string::npos);
    CHECK(source.find("finalizeTopLevelSync") == std::string::npos);
    CHECK(source.find("abortTopLevelSync") == std::string::npos);
    std::printf("[test] Home skips unsupported library views before persistence OK\n");
}

static void testHomeOptionalRailFailuresDoNotStopCatalogPopulation()
{
    std::printf("[test] Home optional rail failures are isolated\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("bool optionalRailFailed") != std::string::npos);
    CHECK(source.find("bool catalogRefreshFailed") != std::string::npos);
    CHECK(source.find("optionalRailFailed=true") != std::string::npos);
    CHECK(source.find("catalogRefreshFailed = true") != std::string::npos);
    CHECK(source.find("if (catalogRefreshFailed) break;") != std::string::npos);
    CHECK(source.find("completeTelemetry(catalogRefreshFailed ? Outcome::Failure")
          != std::string::npos);
    CHECK(source.find("if (optionalRailFailed) break") == std::string::npos);
    std::printf("[test] Home optional rail failures are isolated OK\n");
}

static void testCatalogScopeConfiguredBeforeHomePopulation()
{
    std::printf("[test] Catalog scope precedes Home population\n");
    const std::string appSource = readTestBytes("src/app/App.cpp");
    const auto configure = appSource.find("configureCatalogScopeForSession();");
    const auto home = appSource.find("goToHome();", configure);
    CHECK(configure != std::string::npos && home != std::string::npos);
    CHECK(appSource.find("catalog_scope_deferred_until_media_navigation")
          == std::string::npos);
    std::printf("[test] Catalog scope precedes Home population OK\n");
}

static void testHomePublishesAfterFirstBoundedPage()
{
    std::printf("[test] Home publishes after first bounded page\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    const std::string lifecycle = readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(source.find("m_fetchReady") != std::string::npos);
    CHECK(source.find("m_fetchComplete") != std::string::npos);
    CHECK(source.find("first bounded page") != std::string::npos);
    CHECK(source.find("m_fetchReady.store(true)") != std::string::npos);
    CHECK(lifecycle.find("if (m_fetchReady.load())") != std::string::npos);
    CHECK(source.find("m_fetchDone.store(true);\n                    }")
          == std::string::npos);
    std::printf("[test] Home publishes after first bounded page OK\n");
}

static void testHomePreservesAnimeMembershipDuringBoundedReads()
{
    std::printf("[test] Home preserves Anime membership during bounded reads\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(source.find("m_animeItemIds") != std::string::npos);
    CHECK(source.find("isAnimeSeries") != std::string::npos);
    CHECK(source.find("view.name") != std::string::npos);
    std::printf("[test] Home preserves Anime membership during bounded reads OK\n");
}

static void testHomeUsesCatalogBeforeNetworkRefresh()
{
    std::printf("[test] Home uses CatalogDb before network refresh\n");
    const std::string source = readTestBytes("src/ui/screens/HomeScreen.cpp");
    const std::string syncSource = readTestBytes("src/ui/screens/HomeScreenSync.cpp");
    CHECK(syncSource.find("configureScope") == std::string::npos);
    CHECK(syncSource.find("scopeEpoch == 0") == std::string::npos);
    CHECK(source.find("requestFetch(SDL_GetTicks())") != std::string::npos);
    CHECK(source.find("first_useful_home_ready") == std::string::npos);
    const auto enter = source.find("void HomeScreen::enter()");
    const auto leave = source.find("void HomeScreen::leave()", enter);
    CHECK(enter != std::string::npos && leave != std::string::npos);
    if (enter != std::string::npos && leave != std::string::npos)
        CHECK(source.substr(enter, leave - enter).find(
                  "m_loadState = LoadState::Ready") == std::string::npos);
    std::printf("[test] Home uses CatalogDb before network refresh OK\n");
}

} // namespace
