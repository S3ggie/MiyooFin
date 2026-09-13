#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "../../src/catalog/CatalogPrimitives.hpp"
#include "../../src/catalog/CatalogCompatibility.hpp"

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
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, url, user));
        CHECK(db.upsertSeriesHierarchy(series,
                                       {season},
                                       {{season.id, {episode}}}, 1, 100)
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
        checkCatalogParityItems({season},
                                sqliteKeepSeasons.items);
        checkCatalogParityItems({episode},
                                sqliteKeepEpisodes.items);
    }

    const std::string mediaUrl = url + "-media";
    const auto mediaPaths = catalogMigrationTestPaths(mediaUrl, user);
    removeCatalogMigrationTestPaths(mediaPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, mediaUrl, user));
        const auto generationA = db.beginTopLevelSync(10).get();
        CHECK(generationA.success);
        CatalogDbMediaPageWrite first;
        first.items = {
            catalogParityItem("__task20_removed_item__", "movie",
                              "Removed Item"),
            catalogParityItem("__task20_remaining_item__", "movie",
                              "Remaining Item")};
        first.viewId = "task20-movies";
        first.viewName = "Task 20 Movies";
        first.collectionType = "movies";
        first.syncGeneration = 10;
        CHECK(db.upsertMediaPage(first).get().success);
        CHECK(db.finalizeTopLevelSync(10).get().success);

        const auto generationB = db.beginTopLevelSync(11).get();
        CHECK(generationB.success);
        CatalogDbMediaPageWrite second = first;
        second.items = {first.items[1]};
        second.syncGeneration = 11;
        CHECK(db.upsertMediaPage(second).get().success);
        CHECK(db.finalizeTopLevelSync(11).get().success);
        const auto page = db.readMediaPage("movie", -1, 8).get();
        CHECK(page.success && page.items.size() == 1
              && page.items[0].id == "__task20_remaining_item__");
        CHECK(page.membershipsByItem.size() == 1
              && page.membershipsByItem.at(
                     "__task20_remaining_item__").size() == 1);
    }
    removeCatalogMigrationTestPaths(mediaPaths);

    const std::string deletedViewUrl = url + "-deleted-view";
    const auto deletedViewPaths =
        catalogMigrationTestPaths(deletedViewUrl, user);
    removeCatalogMigrationTestPaths(deletedViewPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, deletedViewUrl, user));
        MediaItem surviving = catalogParityItem(
            "__task21_surviving_view_item__", "movie", "Surviving View Item");
        MediaItem deletedOnly = catalogParityItem(
            "__task21_deleted_view_item__", "movie", "Deleted View Item");

        CHECK(db.beginTopLevelSync(20).get().success);
        CatalogDbMediaPageWrite survivingView;
        survivingView.items = {surviving};
        survivingView.viewId = "task21-surviving-view";
        survivingView.viewName = "Task 21 Surviving View";
        survivingView.collectionType = "movies";
        survivingView.viewOrdinal = 0;
        survivingView.syncGeneration = 20;
        CHECK(db.upsertMediaPage(survivingView).get().success);
        CatalogDbMediaPageWrite deletedView = survivingView;
        deletedView.items = {surviving, deletedOnly};
        deletedView.viewId = "task21-deleted-view";
        deletedView.viewName = "Task 21 Deleted View";
        deletedView.viewOrdinal = 1;
        CHECK(db.upsertMediaPage(deletedView).get().success);
        CHECK(db.finalizeTopLevelSync(20).get().success);

        CHECK(db.beginTopLevelSync(21).get().success);
        survivingView.syncGeneration = 21;
        CHECK(db.upsertMediaPage(survivingView).get().success);
        CHECK(db.finalizeTopLevelSync(21).get().success);

        const auto descriptors = CatalogCompatibility::readLibrarySnapshot(
            db, {0, db.scopeState().requestedEpoch, {}})
                                     .get();
        CHECK(descriptors.success && descriptors.snapshot.movies.size() == 1);
        if (descriptors.success && descriptors.snapshot.movies.size() == 1) {
            CHECK(descriptors.snapshot.movies[0].id
                  == "task21-surviving-view");
            CHECK(descriptors.snapshot.movies[0].name
                  == "Task 21 Surviving View");
            CHECK(descriptors.snapshot.movies[0].items.size() == 1);
            CHECK(descriptors.snapshot.movies[0].items[0].id == surviving.id);
        }

        const auto page = db.readMediaPage("movie", -1, 8).get();
        CHECK(page.success && page.items.size() == 1
              && page.items[0].id == surviving.id);
        CHECK(page.membershipsByItem.size() == 1
              && page.membershipsByItem.at(surviving.id).size() == 1
              && page.membershipsByItem.at(surviving.id)[0].viewId
                     == "task21-surviving-view");
    }
    removeCatalogMigrationTestPaths(deletedViewPaths);

    const std::string movedItemUrl = url + "-moved-item";
    const auto movedItemPaths = catalogMigrationTestPaths(movedItemUrl, user);
    removeCatalogMigrationTestPaths(movedItemPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, movedItemUrl, user));
        MediaItem moved = catalogParityItem(
            "__task22_moved_item__", "movie", "Moved Library Item");
        MediaItem stable = catalogParityItem(
            "__task22_stable_item__", "movie", "Stable Library Item");

        CHECK(db.beginTopLevelSync(30).get().success);
        CatalogDbMediaPageWrite viewA;
        viewA.items = {moved};
        viewA.viewId = "task22-view-a";
        viewA.viewName = "Task 22 View A";
        viewA.collectionType = "movies";
        viewA.viewOrdinal = 0;
        viewA.syncGeneration = 30;
        CHECK(db.upsertMediaPage(viewA).get().success);
        CHECK(db.finalizeTopLevelSync(30).get().success);

        CHECK(db.beginTopLevelSync(31).get().success);
        CatalogDbMediaPageWrite viewB = viewA;
        viewB.items = {moved, stable};
        viewB.viewId = "task22-view-b";
        viewB.viewName = "Task 22 View B";
        viewB.syncGeneration = 31;
        CHECK(db.upsertMediaPage(viewB).get().success);
        CHECK(db.finalizeTopLevelSync(31).get().success);

        const auto firstPage = db.readMediaPage("movie", -1, 1).get();
        CHECK(firstPage.success && firstPage.items.size() == 1
              && firstPage.items[0].id == moved.id && firstPage.hasMore);
        CHECK(firstPage.membershipsByItem.count(moved.id) == 1
              && firstPage.membershipsByItem.at(moved.id).size() == 1
              && firstPage.membershipsByItem.at(moved.id)[0].viewId
                     == "task22-view-b");
        if (firstPage.success && firstPage.hasMore) {
            const auto secondPage =
                db.readMediaPage("movie", -1, 1, firstPage.next).get();
            CHECK(secondPage.success && secondPage.items.size() == 1
                  && secondPage.items[0].id == stable.id
                  && !secondPage.hasMore);
        }
    }
    removeCatalogMigrationTestPaths(movedItemPaths);
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
    const auto connection = db.connectionStateForTest();
    CHECK(connection.open && connection.workerOwned);
    const std::string catalogHeader = readTestBytes("src/catalog/CatalogDb.hpp");
    const std::string catalogSource = readTestBytes("src/catalog/CatalogDb.cpp");
    const std::string compatibilityHeader =
        readTestBytes("src/catalog/CatalogCompatibility.hpp");
    const std::string compatibilitySource =
        readTestBytes("src/catalog/CatalogCompatibility.cpp");
    CHECK(catalogHeader.find("LibrarySnapshot") == std::string::npos);
    CHECK(catalogHeader.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(catalogHeader.find("readLibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("LibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("seedLibrarySnapshot") == std::string::npos);
    CHECK(catalogSource.find("readLibrarySnapshot") == std::string::npos);
    CHECK(compatibilityHeader.find("class CatalogCompatibility")
          != std::string::npos);
    CHECK(compatibilitySource.find("enqueueLibrarySeed")
          != std::string::npos);
    CHECK(compatibilitySource.find("enqueueLibraryRead")
          != std::string::npos);
    CHECK(compatibilitySource.find("sqlite3_") == std::string::npos);
    const char *runtimeSources[] = {
        "src/app/App.cpp", "src/ui/screens/HomeScreen.cpp",
        "src/ui/screens/SeriesScreen.cpp",
        "src/ui/screens/EpisodeBrowserScreen.cpp",
        "src/download/DownloadManager.cpp", "src/library/LibrarySync.cpp",
        "src/library/LibraryQuery.cpp", "src/library/OfflineLibraryQuery.cpp"};
    for (const char *path : runtimeSources) {
        const std::string source = readTestBytes(path);
        CHECK(source.find("seedLibrarySnapshot") == std::string::npos);
        CHECK(source.find("readLibrarySnapshot") == std::string::npos);
    }
    LibrarySnapshot snapshot;
    MediaItem movie = catalogParityItem("__task29_movie__", "movie", "Seed Movie");
    MediaItem secondMovie = catalogParityItem("__task29_movie_2__", "movie", "Seed Movie 2");
    snapshot.movies.push_back({"movies", "Movies", "movies", {movie}});
    snapshot.movies[0].items.push_back(secondMovie);
    snapshot.continueWatching.push_back(movie);
    auto first = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(first.success && first.workerOwned && first.itemsUpserted == 2
          && first.viewsWritten == 1 && first.homeItemsWritten == 1);
    auto read = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    CHECK(read.success && read.snapshot.movies.size() == 1
          && read.snapshot.movies[0].collectionType == "movies"
          && read.snapshot.continueWatching.size() == 1
          && read.snapshot.continueWatching[0].id == movie.id);
    auto second = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(second.success && second.itemsUpserted == 2 && second.viewsWritten == 1);
    auto failed = CatalogCompatibility::seedLibrarySnapshotForTest(
        db, snapshot, 1, {0, epoch, {}}).get();
    CHECK(!failed.success && failed.error == CatalogDbErrorCategory::SqliteError);
    auto recovered = CatalogCompatibility::seedLibrarySnapshot(
        db, LibrarySnapshot{}, {0, epoch, {}}).get();
    CHECK(recovered.success && recovered.itemsUpserted == 0);
    auto nextEpoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    auto stale = CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get();
    CHECK(!stale.success && stale.superseded);
    auto empty = CatalogCompatibility::seedLibrarySnapshot(
        db, LibrarySnapshot{}, {0, nextEpoch, {}}).get();
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
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);
    auto before = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
    auto begin = db.beginTopLevelSync(101, {0, epoch, {}}).get();
    CHECK(begin.success && begin.workerOwned && begin.generation == 101);
    auto newer = db.beginTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(newer.success && newer.generation == 102);
    auto abort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(abort.success && abort.workerOwned);
    auto repeatAbort = db.abortTopLevelSync(102, {0, epoch, {}}).get();
    CHECK(repeatAbort.success);
    auto after = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
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
    auto published = CatalogCompatibility::readLibrarySnapshot(
        db, {0, epoch, {}}).get();
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

    const std::string rollbackUrl = url + "-task28";
    const std::string rollbackUser = user + "-task28";
    const auto rollbackPaths =
        catalogMigrationTestPaths(rollbackUrl, rollbackUser);
    removeCatalogMigrationTestPaths(rollbackPaths);
    {
        auto rollbackDb = std::make_shared<CatalogDb>();
        const auto rollbackEpoch =
            rollbackDb->configureScope(rollbackUrl, rollbackUser);
        CHECK(rollbackDb->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = rollbackUrl;
        session.userId = rollbackUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, rollbackDb, rollbackEpoch);

        CHECK(sync->begin(200).get().success);
        CatalogDbMediaPageWrite generationAMovies;
        generationAMovies.items = {
            catalogParityItem("__task28_a_movie_1__", "movie", "A Movie 1"),
            catalogParityItem("__task28_a_movie_2__", "movie", "A Movie 2")};
        generationAMovies.viewId = "task28-a-movies";
        generationAMovies.viewName = "A Movies";
        generationAMovies.collectionType = "movies";
        generationAMovies.viewOrdinal = 0;
        generationAMovies.syncGeneration = 200;
        CHECK(sync->stage(generationAMovies).get().success);
        CatalogDbMediaPageWrite generationAShows = generationAMovies;
        generationAShows.items = {catalogParityItem(
            "__task28_a_show__", "show", "A Show")};
        generationAShows.viewId = "task28-a-shows";
        generationAShows.viewName = "A Shows";
        generationAShows.collectionType = "tvshows";
        generationAShows.viewOrdinal = 1;
        CHECK(sync->stage(generationAShows).get().success);
        CHECK(sync->finalize(200).get().success);

        library::LibraryQuery query(rollbackDb, rollbackEpoch);
        const auto beforeMovies = query.movies(-1, 8).get();
        const auto beforeShows = query.shows(-1, 8).get();
        CHECK(beforeMovies.success && beforeMovies.items.size() == 2);
        CHECK(beforeShows.success && beforeShows.items.size() == 1
              && beforeShows.items[0].id == "__task28_a_show__");

        CHECK(sync->begin(201).get().success);
        CatalogDbMediaPageWrite generationB = generationAMovies;
        generationB.items = {catalogParityItem(
            "__task28_b_movie__", "movie", "B Movie")};
        generationB.viewId = "task28-b-movies";
        generationB.viewName = "B Movies";
        generationB.syncGeneration = 201;
        CHECK(sync->stage(generationB).get().success);
        CHECK(sync->abort(201).get().success);

        const auto afterCancelMovies = query.movies(-1, 8).get();
        const auto afterCancelShows = query.shows(-1, 8).get();
        CHECK(afterCancelMovies.success && afterCancelMovies.items.size() == 2);
        CHECK(afterCancelShows.success && afterCancelShows.items.size() == 1
              && afterCancelShows.items[0].id == "__task28_a_show__");
        CHECK(std::none_of(afterCancelMovies.items.begin(),
                           afterCancelMovies.items.end(),
                           [](const MediaItem &item) {
                               return item.id == "__task28_b_movie__";
                           }));

        CHECK(sync->begin(202).get().success);
        CatalogDbMediaPageWrite generationC = generationAMovies;
        generationC.items = {catalogParityItem(
            "__task28_c_movie__", "movie", "C Movie")};
        generationC.viewId = "task28-c-movies";
        generationC.viewName = "C Movies";
        generationC.syncGeneration = 202;
        CHECK(sync->stage(generationC).get().success);
        CHECK(sync->finalize(202).get().success);
        const auto afterC = query.movies(-1, 8).get();
        CHECK(afterC.success && afterC.items.size() == 1
              && afterC.items[0].id == "__task28_c_movie__");
    }
    removeCatalogMigrationTestPaths(rollbackPaths);
    removeCatalogMigrationTestPaths(paths);
}

void testLibrarySyncTeardownDuringStagedGeneration()
{
    std::printf("[test] LibrarySync teardown during staged generation\n");
    const std::string url =
        "https://sqlite-library-sync-teardown-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string user = "library-sync-teardown-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    Session session;
    session.serverUrl = url;
    session.userId = user;
    {
        auto sync = std::make_unique<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(300).get().success);
        CatalogDbMediaPageWrite committedPage;
        committedPage.items = {catalogParityItem(
            "__task30_committed_movie__", "movie", "Committed Movie")};
        committedPage.viewId = "task30-committed-movies";
        committedPage.viewName = "Committed Movies";
        committedPage.collectionType = "movies";
        committedPage.viewOrdinal = 0;
        committedPage.syncGeneration = 300;
        CHECK(sync->stage(committedPage).get().success);
        CHECK(sync->finalize(300).get().success);
        CHECK(sync->writeSyncState(300, 300, 300).get().success);

        const auto regressed = sync->writeSyncState(299, 299, 299).get();
        CHECK(!regressed.success
              && regressed.error == CatalogDbErrorCategory::ConfigurationFailed);
        const auto afterRegression = db->readSyncState(
            false, 0, 0, {0, epoch, {}}).get();
        CHECK(afterRegression.success
              && afterRegression.lastSuccessfulMs == 300
              && afterRegression.lastReconcileMs == 300
              && afterRegression.committedGeneration == 300);

        auto cancelledCheckpoint = std::make_shared<std::atomic_bool>(true);
        const auto cancelled = sync->writeSyncState(
            400, 400, 400, cancelledCheckpoint).get();
        CHECK(!cancelled.success && cancelled.cancelled);
        const auto afterCancellation = db->readSyncState(
            false, 0, 0, {0, epoch, {}}).get();
        CHECK(afterCancellation.success
              && afterCancellation.lastSuccessfulMs == 300
              && afterCancellation.lastReconcileMs == 300
              && afterCancellation.committedGeneration == 300);

        db->setWorkerPausedForTest(true);
        auto pendingBegin = sync->begin(301);
        CatalogDbMediaPageWrite stagedPage = committedPage;
        stagedPage.items = {catalogParityItem(
            "__task30_unpublished_movie__", "movie", "Unpublished Movie")};
        stagedPage.viewId = "task30-unpublished-movies";
        stagedPage.viewName = "Unpublished Movies";
        stagedPage.syncGeneration = 301;
        auto pendingStage = sync->stage(stagedPage);
        sync.reset();

        db->setWorkerPausedForTest(false);
        const auto beginResult = pendingBegin.get();
        const auto stageResult = pendingStage.get();
        CHECK(beginResult.success && beginResult.generation == 301);
        CHECK(!stageResult.success
              && (stageResult.cancelled || stageResult.superseded));
    }

    db.reset();
    auto reopened = std::make_shared<CatalogDb>();
    const auto reopenedEpoch = reopened->configureScope(url, user);
    CHECK(reopened->waitForIdleForTest(std::chrono::seconds(2)));
    library::LibraryQuery query(reopened, reopenedEpoch);
    const auto movies = query.movies(-1, 8).get();
    CHECK(movies.success && movies.items.size() == 1
          && movies.items[0].id == "__task30_committed_movie__");
    const auto syncState = reopened->readSyncState(
        false, 0, 0, {0, reopenedEpoch, {}}).get();
    CHECK(syncState.success && syncState.lastSuccessfulMs == 300
          && syncState.lastReconcileMs == 300
          && syncState.committedGeneration == 300);
    reopened.reset();
    removeCatalogMigrationTestPaths(paths);

    const std::string otherUrl =
        "https://sqlite-library-sync-scope-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const auto otherPaths = catalogMigrationTestPaths(otherUrl, user);
    removeCatalogMigrationTestPaths(otherPaths);
    auto otherDb = std::make_shared<CatalogDb>();
    const auto otherEpoch = otherDb->configureScope(otherUrl, user);
    CHECK(otherDb->waitForIdleForTest(std::chrono::seconds(2)));
    Session otherSession = session;
    otherSession.serverUrl = otherUrl;
    auto otherSync = std::make_unique<library::LibrarySync>(
        otherSession, otherDb, otherEpoch);
    CHECK(otherSync->writeSyncState(900, 900, 1).get().success);
    const auto otherState = otherDb->readSyncState(
        false, 0, 0, {0, otherEpoch, {}}).get();
    CHECK(otherState.success && otherState.lastSuccessfulMs == 900
          && otherState.committedGeneration == 1);
    otherSync.reset();
    otherDb.reset();
    removeCatalogMigrationTestPaths(otherPaths);
    std::printf("[test] LibrarySync teardown during staged generation OK\n");
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
    CHECK(LibraryCache::scopeKey(url, user) == "184a8ca0cca21a05");
    CHECK(miyoofin::catalog::scopeKey(url, user) == "184a8ca0cca21a05");

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
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);

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
    for (const auto &item : movies) {
        CHECK(organizationalSortKey(item.title)
              == miyoofin::catalog::organizationalSortKey(item.title));
        CHECK(alphabetIndex(item.title)
              == miyoofin::catalog::alphabetIndex(item.title));
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
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);
    expected = snapshot.movies[0].items;
    std::sort(expected.begin(), expected.end(), organizationalLess);
    expectedIds.clear();
    for (const auto &item : expected) expectedIds.push_back(item.id);
    CHECK(collectCatalogPageIds(db, "movie", -1, 2, epoch) == expectedIds);

    const auto plan = db.runMediaPageQueryPlanForTest("movie", -1);
    CHECK(plan.success && plan.workerOwned && plan.mediaPageUsesSortIndex
          && plan.mediaPageAvoidsTempSort);

    const std::string catalogSource = readTestBytes("src/catalog/CatalogDb.cpp");
    const std::string catalogHeader = readTestBytes("src/catalog/CatalogDb.hpp");
    CHECK(catalogSource.find("JellyfinApi") == std::string::npos);
    CHECK(catalogSource.find("LibraryCache::") == std::string::npos);
    CHECK(catalogSource.find("TitleOrganization") == std::string::npos);
    CHECK(catalogHeader.find("JellyfinApi") == std::string::npos);
    CHECK(catalogHeader.find("LibraryCache") == std::string::npos);
    CHECK(catalogHeader.find("TitleOrganization") == std::string::npos);
    CHECK(catalogSource.find("CatalogPrimitives.hpp") != std::string::npos);

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
    CHECK(CatalogCompatibility::seedLibrarySnapshot(
        db, snapshot, {0, epoch, {}}).get().success);

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

    const std::string multiViewUrl =
        "https://sqlite-multiple-tv-views-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const auto multiViewPaths = catalogMigrationTestPaths(multiViewUrl,
                                                          "media-membership-user");
    removeCatalogMigrationTestPaths(multiViewPaths);
    {
        CatalogDb db;
        CHECK(configureCatalogParityDb(db, multiViewUrl,
                                       "media-membership-user"));
        MediaItem shared = catalogParityItem(
            "__task23_shared_series__", "show", "Shared Series");
        MediaItem firstOnly = catalogParityItem(
            "__task23_first_only_series__", "show", "First Library Series");
        MediaItem secondOnly = catalogParityItem(
            "__task23_second_only_series__", "show", "Second Library Series");

        CHECK(db.beginTopLevelSync(40).get().success);
        CatalogDbMediaPageWrite firstView;
        firstView.items = {shared, firstOnly};
        firstView.viewId = "task23-first-tv-view";
        firstView.viewName = "Task 23 First TV View";
        firstView.collectionType = "tvshows";
        firstView.viewOrdinal = 0;
        firstView.syncGeneration = 40;
        CHECK(db.upsertMediaPage(firstView).get().success);
        CatalogDbMediaPageWrite secondView = firstView;
        secondView.items = {shared, secondOnly};
        secondView.viewId = "task23-second-tv-view";
        secondView.viewName = "Task 23 Second TV View";
        secondView.viewOrdinal = 1;
        CHECK(db.upsertMediaPage(secondView).get().success);
        CHECK(db.finalizeTopLevelSync(40).get().success);

        const auto page = db.readMediaPage("show", -1, 8).get();
        CHECK(page.success && page.items.size() == 3 && !page.hasMore);
        std::set<std::string> ids;
        for (const auto &item : page.items) ids.insert(item.id);
        CHECK(ids.size() == 3 && ids.count(shared.id) == 1
              && ids.count(firstOnly.id) == 1 && ids.count(secondOnly.id) == 1);
        CHECK(page.membershipsByItem.at(shared.id).size() == 2);
        CHECK(page.membershipsByItem.at(firstOnly.id).size() == 1);
        CHECK(page.membershipsByItem.at(secondOnly.id).size() == 1);
        const auto &sharedMemberships = page.membershipsByItem.at(shared.id);
        CHECK(sharedMemberships[0].viewId == firstView.viewId
              && sharedMemberships[1].viewId == secondView.viewId);
        CHECK(page.membershipsByItem.at(secondOnly.id)[0].viewId
              == secondView.viewId);
    }
    removeCatalogMigrationTestPaths(multiViewPaths);

    const std::string animeUrl =
        "https://sqlite-anime-membership-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string animeUser = "anime-membership-user";
    const auto animePaths = catalogMigrationTestPaths(animeUrl, animeUser);
    removeCatalogMigrationTestPaths(animePaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(animeUrl, animeUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = animeUrl;
        session.userId = animeUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);

        MediaItem animeByView = catalogParityItem(
            "__task24_anime_by_view__", "show", "Anime By View");
        animeByView.genres = {"Action"};
        animeByView.imageTags["Primary"] = "task24-view-primary";
        MediaItem animeByGenre = catalogParityItem(
            "__task24_anime_by_genre__", "show", "Anime By Genre");
        animeByGenre.genres = {"Anime"};
        animeByGenre.imageTags["Primary"] = "task24-genre-primary";
        MediaItem control = catalogParityItem(
            "__task24_control_show__", "show", "Control Show");
        control.genres = {"Drama"};
        control.imageTags["Primary"] = "task24-control-primary";
        MediaItem shared = catalogParityItem(
            "__task24_normal_and_anime__", "show", "Normal And Anime");
        shared.genres = {"Drama"};
        shared.imageTags["Primary"] = "task24-shared-primary";

        CHECK(sync->begin(50).get().success);
        CatalogDbMediaPageWrite shows;
        shows.items = {animeByGenre, control, shared};
        shows.viewId = "task24-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        shows.viewOrdinal = 0;
        shows.syncGeneration = 50;
        CHECK(sync->stage(shows).get().success);
        CatalogDbMediaPageWrite anime = shows;
        anime.items = {animeByView, shared};
        anime.viewId = "task24-anime";
        anime.viewName = "Anime Library";
        anime.viewOrdinal = 1;
        CHECK(sync->stage(anime).get().success);
        CHECK(sync->finalize(50).get().success);

        library::LibraryQuery query(db, epoch);
        const auto page = query.shows(-1, 8).get();
        CHECK(page.success && page.items.size() == 4 && !page.hasMore);
        std::map<std::string, MediaItem> items;
        for (const auto &item : page.items) items[item.id] = item;
        CHECK(items.size() == 4);
        CHECK(items.at(animeByView.id).imageTags.at("Primary")
              == "task24-view-primary");
        CHECK(items.at(animeByGenre.id).imageTags.at("Primary")
              == "task24-genre-primary");
        CHECK(items.at(control.id).imageTags.at("Primary")
              == "task24-control-primary");
        CHECK(items.at(shared.id).imageTags.at("Primary")
              == "task24-shared-primary");

        const auto animeFor = [&](const MediaItem &item) {
            const auto &memberships = page.membershipsByItem.at(item.id);
            return std::any_of(
                memberships.begin(), memberships.end(),
                [&](const CatalogDbMediaPageMembership &membership) {
                    return isAnimeSeries(membership.viewName, item);
                });
        };
        CHECK(animeFor(items.at(animeByView.id)));
        CHECK(animeFor(items.at(animeByGenre.id)));
        CHECK(!animeFor(items.at(control.id)));
        CHECK(animeFor(items.at(shared.id)));
        CHECK(page.membershipsByItem.at(shared.id).size() == 2);

        const auto animePage = query.anime(-1, 8).get();
        CHECK(animePage.success && animePage.items.size() == 3
              && !animePage.hasMore);
        std::set<std::string> animeIds;
        for (const auto &item : animePage.items) animeIds.insert(item.id);
        CHECK(animeIds.size() == 3
              && animeIds.count(animeByView.id) == 1
              && animeIds.count(animeByGenre.id) == 1
              && animeIds.count(shared.id) == 1
              && animeIds.count(control.id) == 0);
    }
    removeCatalogMigrationTestPaths(animePaths);

    const std::string playlistUrl =
        "https://sqlite-playlist-interleave-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string playlistUser = "playlist-interleave-user";
    const auto playlistPaths =
        catalogMigrationTestPaths(playlistUrl, playlistUser);
    removeCatalogMigrationTestPaths(playlistPaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(playlistUrl, playlistUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = playlistUrl;
        session.userId = playlistUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        MediaItem movie = catalogParityItem(
            "__task25_supported_movie__", "movie", "Supported Movie");
        MediaItem playlistOnly = catalogParityItem(
            "__task25_playlist_only__", "movie", "Playlist Only Item");
        MediaItem show = catalogParityItem(
            "__task25_later_tv_show__", "show", "Later TV Show");

        CHECK(sync->begin(60).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {movie};
        movies.viewId = "task25-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.viewOrdinal = 0;
        movies.syncGeneration = 60;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite playlist = movies;
        playlist.items = {movie, playlistOnly};
        playlist.viewId = "task25-playlist";
        playlist.viewName = "Watch Playlist";
        playlist.collectionType = "playlists";
        playlist.viewOrdinal = 1;
        CHECK(sync->stage(playlist).get().success);
        CatalogDbMediaPageWrite tv = movies;
        tv.items = {show};
        tv.viewId = "task25-tv";
        tv.viewName = "TV Shows";
        tv.collectionType = "tvshows";
        tv.viewOrdinal = 2;
        CHECK(sync->stage(tv).get().success);
        CHECK(sync->finalize(60).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == movie.id);
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == show.id);
        const auto &movieMemberships = moviePage.membershipsByItem.at(movie.id);
        const bool hasSupportedMovieMembership = std::any_of(
            movieMemberships.begin(), movieMemberships.end(),
            [&](const CatalogDbMediaPageMembership &membership) {
                return membership.viewId == movies.viewId
                    && membership.collectionType == "movies";
            });
        CHECK(hasSupportedMovieMembership);
        CHECK(moviePage.membershipsByItem.count(playlistOnly.id) == 0);
        CHECK(showPage.membershipsByItem.count(playlistOnly.id) == 0);
    }
    removeCatalogMigrationTestPaths(playlistPaths);
    std::printf("[test] CatalogDb media pages preserve TV view membership OK\n");
}

void testLibraryCacheHomeParityHarness()
{
    const std::string hierarchySource =
        readTestBytes("src/ui/screens/HomeScreenHierarchy.cpp");
    CHECK(hierarchySource.find("getSeasons(") == std::string::npos);
    CHECK(hierarchySource.find("getEpisodes(") == std::string::npos);
    CHECK(hierarchySource.find("submitCatalogHierarchy") == std::string::npos);
    CHECK(hierarchySource.find("m_libraryQuery->seasons")
          != std::string::npos);
    CHECK(hierarchySource.find("m_libraryQuery->episodes")
          != std::string::npos);
    CHECK(hierarchySource.find("m_librarySync->refreshSeasons")
          != std::string::npos);
    CHECK(hierarchySource.find("m_librarySync->refreshEpisodes")
          != std::string::npos);
    const std::string lifecycleSource =
        readTestBytes("src/ui/screens/HomeScreen.cpp");
    CHECK(lifecycleSource.find("submitCatalogHierarchy")
          == std::string::npos);
    CHECK(lifecycleSource.find("m_catalogDb->stageSeriesHierarchy")
          == std::string::npos);

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

static void testLegacyWholeFilePersistenceRetired()
{
    const std::string libraryCacheSource =
        readTestBytes("src/cache/LibraryCache.cpp");
    const std::string syncStateSource =
        readTestBytes("src/cache/SyncState.cpp");
    const std::string offlineCatalogHeader =
        readTestBytes("src/cache/OfflineCatalog.hpp");
    const std::string makefile = readTestBytes("Makefile");
    const std::string crossMakefile = readTestBytes("Makefile.cross");

    CHECK(libraryCacheSource.find("LibraryCache::save")
          == std::string::npos);
    CHECK(syncStateSource.find("SyncStateStore::save")
          == std::string::npos);
    CHECK(offlineCatalogHeader.find("OfflineCatalog::")
          == std::string::npos);
    CHECK(::access("src/cache/OfflineCatalog.cpp", F_OK) != 0);
    CHECK(makefile.find("src/cache/OfflineCatalog.cpp")
          == std::string::npos);
    CHECK(crossMakefile.find("src/cache/OfflineCatalog.cpp")
          == std::string::npos);
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
    auto snapshot = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
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
    auto before = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
    CHECK(before.success && before.snapshot.movies.size() == 1
          && before.snapshot.movies[0].items[0].id == "live-stage");
    auto stale = staged; stale.syncGeneration = 76;
    CHECK(db.upsertMediaPage(stale, metadata).get().superseded);
    auto failed = staged; failed.items = {catalogParityItem("rollback-stage", "movie", "R")};
    CHECK(!db.upsertMediaPageForTest(failed, 0, metadata).get().success);
    CHECK(db.abortTopLevelSync(77, metadata).get().success);
    auto after = CatalogCompatibility::readLibrarySnapshot(
        db, metadata).get();
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

    const int resumeListener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(resumeListener >= 0);
    if (resumeListener >= 0) {
        int reuse = 1;
        ::setsockopt(resumeListener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        CHECK(::bind(resumeListener,
                     reinterpret_cast<sockaddr *>(&address),
                     sizeof(address)) == 0);
        socklen_t addressSize = sizeof(address);
        CHECK(::getsockname(resumeListener,
                            reinterpret_cast<sockaddr *>(&address),
                            &addressSize) == 0);
        CHECK(::listen(resumeListener, 1) == 0);
        std::thread resumeServer([&] {
            const int client = ::accept(resumeListener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                ::close(client);
            }
        });
        std::vector<MediaItem> resumeItems;
        std::string resumeError;
        const bool resumeOk = JellyfinApi::getResumeItems(
            "http://127.0.0.1:" +
                std::to_string(ntohs(address.sin_port)),
            "token", "user", "device", 12, resumeItems, resumeError);
        resumeServer.join();
        ::close(resumeListener);
        CHECK(!resumeOk && !resumeError.empty());
    }

    const int latestListener = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(latestListener >= 0);
    if (latestListener >= 0) {
        int reuse = 1;
        ::setsockopt(latestListener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        CHECK(::bind(latestListener,
                     reinterpret_cast<sockaddr *>(&address),
                     sizeof(address)) == 0);
        socklen_t addressSize = sizeof(address);
        CHECK(::getsockname(latestListener,
                            reinterpret_cast<sockaddr *>(&address),
                            &addressSize) == 0);
        CHECK(::listen(latestListener, 1) == 0);
        const std::string body =
            R"({"Items":[{"Id":"__task26_recent__","Type":"Movie","Name":"Recent Item"}]})";
        std::thread latestServer([&] {
            const int client = ::accept(latestListener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                const std::string response =
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                    "Content-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body;
                (void)::send(client, response.data(), response.size(), 0);
                ::close(client);
            }
        });
        std::vector<MediaItem> latestItems;
        std::string latestError;
        const bool latestOk = JellyfinApi::getLatestItems(
            "http://127.0.0.1:" +
                std::to_string(ntohs(address.sin_port)),
            "token", "user", "device", 16, latestItems, latestError);
        latestServer.join();
        ::close(latestListener);
        CHECK(latestOk && latestError.empty() && latestItems.size() == 1
              && latestItems[0].id == "__task26_recent__");
    }

    const std::string catalogUrl =
        "https://sqlite-continue-watching-failure-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string catalogUser = "continue-watching-failure-user";
    const auto paths = catalogMigrationTestPaths(catalogUrl, catalogUser);
    removeCatalogMigrationTestPaths(paths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(catalogUrl, catalogUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = catalogUrl;
        session.userId = catalogUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(70).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {catalogParityItem(
            "__task26_catalog_movie__", "movie", "Catalog Movie")};
        movies.viewId = "task26-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.syncGeneration = 70;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite shows = movies;
        shows.items = {catalogParityItem(
            "__task26_catalog_show__", "show", "Catalog Show")};
        shows.viewId = "task26-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        CHECK(sync->stage(shows).get().success);
        CHECK(sync->finalize(70).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == "__task26_catalog_movie__");
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == "__task26_catalog_show__");
    }
    removeCatalogMigrationTestPaths(paths);

    const auto runRailFixture = [](bool latest, bool respond,
                                   std::vector<MediaItem> &items,
                                   std::string &error) {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener < 0) return false;
        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(0);
        if (::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) != 0) {
            ::close(listener);
            return false;
        }
        socklen_t addressSize = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                          &addressSize) != 0
            || ::listen(listener, 1) != 0) {
            ::close(listener);
            return false;
        }
        const std::string body = latest
            ? R"({"Items":[{"Id":"__task27_latest__","Type":"Movie","Name":"Latest Item"}]})"
            : R"({"Items":[{"Id":"__task27_resume__","Type":"Movie","Name":"Resume Item"}]})";
        std::thread server([&] {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client >= 0) {
                char buffer[2048];
                (void)::recv(client, buffer, sizeof(buffer), 0);
                if (respond) {
                    const std::string response =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
                    (void)::send(client, response.data(), response.size(), 0);
                }
                ::close(client);
            }
        });
        const std::string url = "http://127.0.0.1:" +
            std::to_string(ntohs(address.sin_port));
        const bool ok = latest
            ? JellyfinApi::getLatestItems(url, "token", "user", "device",
                                           16, items, error)
            : JellyfinApi::getResumeItems(url, "token", "user", "device",
                                          12, items, error);
        server.join();
        ::close(listener);
        return ok;
    };
    std::vector<MediaItem> resumed;
    std::string resumeError;
    CHECK(runRailFixture(false, true, resumed, resumeError)
          && resumeError.empty() && resumed.size() == 1
          && resumed[0].id == "__task27_resume__");
    std::vector<MediaItem> latest;
    std::string latestError;
    CHECK(!runRailFixture(true, false, latest, latestError)
          && !latestError.empty());

    const std::string recentCatalogUrl =
        "https://sqlite-recently-added-failure-" +
        std::to_string(static_cast<long long>(getpid())) + ".example";
    const std::string recentCatalogUser = "recently-added-failure-user";
    const auto recentPaths =
        catalogMigrationTestPaths(recentCatalogUrl, recentCatalogUser);
    removeCatalogMigrationTestPaths(recentPaths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch =
            db->configureScope(recentCatalogUrl, recentCatalogUser);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = recentCatalogUrl;
        session.userId = recentCatalogUser;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(80).get().success);
        CatalogDbMediaPageWrite movies;
        movies.items = {catalogParityItem(
            "__task27_catalog_movie__", "movie", "Catalog Movie")};
        movies.viewId = "task27-movies";
        movies.viewName = "Movies";
        movies.collectionType = "movies";
        movies.syncGeneration = 80;
        CHECK(sync->stage(movies).get().success);
        CatalogDbMediaPageWrite shows = movies;
        shows.items = {catalogParityItem(
            "__task27_catalog_show__", "show", "Catalog Show")};
        shows.viewId = "task27-shows";
        shows.viewName = "Shows";
        shows.collectionType = "tvshows";
        CHECK(sync->stage(shows).get().success);
        CHECK(sync->finalize(80).get().success);

        library::LibraryQuery query(db, epoch);
        const auto moviePage = query.movies(-1, 8).get();
        const auto showPage = query.shows(-1, 8).get();
        CHECK(moviePage.success && moviePage.items.size() == 1
              && moviePage.items[0].id == "__task27_catalog_movie__");
        CHECK(showPage.success && showPage.items.size() == 1
              && showPage.items[0].id == "__task27_catalog_show__");
    }
    removeCatalogMigrationTestPaths(recentPaths);
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
    CHECK(source.find("CachedLibraryView classificationView") == std::string::npos);
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

    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-warm-home-" + suffix + ".example";
    const std::string user = "warm-home-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    {
        auto db = std::make_shared<CatalogDb>();
        const auto epoch = db->configureScope(url, user);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        Session session;
        session.serverUrl = url;
        session.userId = user;
        auto sync = std::make_shared<library::LibrarySync>(
            session, db, epoch);
        CHECK(sync->begin(1).get().success);

        CatalogDbMediaPageWrite moviePage;
        moviePage.items = {catalogParityItem(
            "__task19_warm_movie__", "movie", "Warm Movie")};
        moviePage.viewId = "warm-movies";
        moviePage.viewName = "Warm Movies";
        moviePage.collectionType = "movies";
        moviePage.syncGeneration = 1;
        CHECK(sync->stage(moviePage).get().success);

        CatalogDbMediaPageWrite showPage;
        showPage.items = {catalogParityItem(
            "__task19_warm_show__", "show", "Warm Show")};
        showPage.viewId = "warm-shows";
        showPage.viewName = "Warm Shows";
        showPage.collectionType = "tvshows";
        showPage.syncGeneration = 1;
        CHECK(sync->stage(showPage).get().success);
        CHECK(sync->finalize(1).get().success);
    }

    {
        auto reopened = std::make_shared<CatalogDb>();
        const auto epoch = reopened->configureScope(url, user);
        CHECK(reopened->waitForIdleForTest(std::chrono::seconds(2)));
        CHECK(reopened->scopeState().openState == CatalogDbOpenState::SupportedV3);
        library::LibraryQuery query(reopened, epoch);
        const auto movies = query.movies(-1, 8).get();
        const auto shows = query.shows(-1, 8).get();
        CHECK(movies.success && movies.items.size() == 1
              && movies.items[0].id == "__task19_warm_movie__");
        CHECK(shows.success && shows.items.size() == 1
              && shows.items[0].id == "__task19_warm_show__");
        CHECK(movies.membershipsByItem.at("__task19_warm_movie__").size() == 1);
        CHECK(shows.membershipsByItem.at("__task19_warm_show__").size() == 1);

        Session session;
        session.serverUrl = url;
        session.userId = user;
        auto sync = std::make_shared<library::LibrarySync>(
            session, reopened, epoch);
        CHECK(sync->begin(2).get().success);
        CHECK(sync->abort(2).get().success);
        const auto afterFailure = query.movies(-1, 8).get();
        CHECK(afterFailure.success && afterFailure.items.size() == 1
              && afterFailure.items[0].id == "__task19_warm_movie__");
    }
    removeCatalogMigrationTestPaths(paths);
    std::printf("[test] Home uses CatalogDb before network refresh OK\n");
}

} // namespace
