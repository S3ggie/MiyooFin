#include <cerrno>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../../vendor/sqlite/sqlite3.h"

namespace {

struct CatalogMigrationTestPaths {
    std::string scope;
    std::string libraryDirectory;
    std::string offlineDirectory;
    std::string finalPath;
    std::string migratingPath;
    std::string legacyPath;
};

CatalogMigrationTestPaths catalogMigrationTestPaths(
    const std::string &url, const std::string &user)
{
    CatalogMigrationTestPaths paths;
    paths.scope = LibraryCache::scopeKey(url, user);
    paths.libraryDirectory = "cache/library/" + paths.scope;
    paths.offlineDirectory = "cache/offline/" + paths.scope;
    paths.finalPath = paths.libraryDirectory + "/catalog.sqlite3";
    paths.migratingPath = paths.finalPath + ".migrating";
    paths.legacyPath = paths.offlineDirectory + "/catalog.v1";
    return paths;
}

void removeCatalogMigrationTestPaths(const CatalogMigrationTestPaths &paths)
{
    const std::string files[] = {
        paths.finalPath, paths.finalPath + "-journal",
        paths.finalPath + "-wal", paths.finalPath + "-shm",
        paths.migratingPath, paths.migratingPath + "-journal",
        paths.migratingPath + "-wal", paths.migratingPath + "-shm",
        paths.legacyPath};
    for (const auto &file : files) {
        std::remove(file.c_str());
    }
    std::remove(paths.libraryDirectory.c_str());
    std::remove(paths.offlineDirectory.c_str());
    ::rmdir(paths.libraryDirectory.c_str());
    ::rmdir(paths.offlineDirectory.c_str());
}

bool makeCatalogMigrationTestDirectory(const std::string &path)
{
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            const std::string directory = path.substr(0, i);
            if (!directory.empty() && ::mkdir(directory.c_str(), 0755)
                && errno != EEXIST) {
                return false;
            }
        }
    }
    return true;
}

void writeCatalogMigrationFile(const std::string &path, const std::string &value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace

static void testCatalogDbFreshBootstrapPathStates()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));

    const std::string freshUrl =
        "https://sqlite-fresh-bootstrap-" + suffix + ".example";
    const std::string freshUser = "fresh-bootstrap-user";
    const auto fresh = catalogMigrationTestPaths(freshUrl, freshUser);
    removeCatalogMigrationTestPaths(fresh);
    const std::string diagnosticPath = "/tmp/miyoofin-catalog-bootstrap-"
        + suffix + ".log";
    std::remove(diagnosticPath.c_str());
    uiDiagnostics().start(diagnosticPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(freshUrl, freshUser);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(state.migration.attempted && state.migration.succeeded);
        CHECK(state.migration.finalPresent);
        CHECK(!state.migration.migratingPresent);
        CHECK(state.migration.files == CatalogDbMigrationFileState::FinalOnly);
        const auto schema = db.runSchemaDiagnosticsForTest();
        CHECK(schema.success && schema.exactSchema
              && schema.applicationId == 0x4D59464E
              && schema.userVersion == 1);
    }
    uiDiagnostics().stop();
    const std::string diagnostics = readTestBytes(diagnosticPath);
    const auto temporaryOpenStarted = diagnostics.rfind(
        "[CatalogDb] bootstrap_temp_open_started");
    const auto temporaryCreated = diagnostics.rfind(
        "[CatalogDb] bootstrap_temp_created");
    const auto temporaryFinalized = diagnostics.rfind(
        "[CatalogDb] bootstrap_temp_finalized final_present=1");
    CHECK(temporaryOpenStarted != std::string::npos);
    CHECK(temporaryCreated != std::string::npos);
    CHECK(temporaryFinalized != std::string::npos);
    CHECK(temporaryOpenStarted < temporaryCreated);
    CHECK(temporaryCreated < temporaryFinalized);
    std::remove(diagnosticPath.c_str());
    removeCatalogMigrationTestPaths(fresh);

    const std::string legacyUrl =
        "https://sqlite-no-legacy-import-" + suffix + ".example";
    const std::string legacyUser = "no-legacy-import-user";
    const auto legacy = catalogMigrationTestPaths(legacyUrl, legacyUser);
    removeCatalogMigrationTestPaths(legacy);
    CHECK(makeCatalogMigrationTestDirectory(legacy.offlineDirectory));
    writeCatalogMigrationFile(legacy.legacyPath,
                              "opaque legacy bytes that are never parsed");
    const std::string legacyBefore = readTestBytes(legacy.legacyPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(legacyUrl, legacyUser);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(state.migration.attempted && state.migration.succeeded);
        CHECK(readTestBytes(legacy.legacyPath) == legacyBefore);
        CHECK(state.migration.finalPresent);
    }
    CHECK(readTestBytes(legacy.legacyPath) == legacyBefore);
    removeCatalogMigrationTestPaths(legacy);

    const std::string staleUrl =
        "https://sqlite-stale-bootstrap-" + suffix + ".example";
    const std::string staleUser = "stale-bootstrap-user";
    const auto stale = catalogMigrationTestPaths(staleUrl, staleUser);
    removeCatalogMigrationTestPaths(stale);
    CHECK(makeCatalogMigrationTestDirectory(stale.libraryDirectory));
    writeCatalogMigrationFile(stale.migratingPath, "incomplete temporary state");
    writeCatalogMigrationFile(stale.migratingPath + "-journal", "stale journal");
    {
        CatalogDb db;
        CHECK(db.configureScope(staleUrl, staleUser) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.ready && state.migration.finalPresent);
        CHECK(!state.migration.migratingPresent);
        CHECK(readTestBytes(stale.migratingPath).empty());
        CHECK(readTestBytes(stale.migratingPath + "-journal").empty());
    }
    removeCatalogMigrationTestPaths(stale);

    const std::string finalUrl =
        "https://sqlite-final-authority-" + suffix + ".example";
    const std::string finalUser = "final-authority-user";
    const auto final = catalogMigrationTestPaths(finalUrl, finalUser);
    removeCatalogMigrationTestPaths(final);
    {
        CatalogDb db;
        CHECK(db.configureScope(finalUrl, finalUser) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        CHECK(db.writeSchemaMarkerForTest("final-authoritative").success);
        CHECK(db.deconfigureScope() != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    }
    CHECK(makeCatalogMigrationTestDirectory(final.offlineDirectory));
    writeCatalogMigrationFile(final.legacyPath, "legacy rollback artifact");
    writeCatalogMigrationFile(final.migratingPath, "must not be opened");
    {
        CatalogDb db;
        CHECK(db.configureScope(finalUrl, finalUser) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.ready && state.migration.finalWins);
        CHECK(!state.migration.migratingPresent);
        CHECK(db.readSchemaMarkerForTest().sentinel == "final-authoritative");
        CHECK(readTestBytes(final.legacyPath) == "legacy rollback artifact");
        CHECK(readTestBytes(final.migratingPath).empty());
    }
    removeCatalogMigrationTestPaths(final);
}

static void testCatalogDbFreshBootstrapUnsupportedFinalPreserved()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-unsupported-final-" + suffix + ".example";
    const std::string user = "unsupported-final-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    {
        CatalogDb db;
        CHECK(db.configureScope(url, user) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        CHECK(db.setSchemaMetadataForTest(0x4D59464E, 99).success);
        CHECK(db.deconfigureScope() != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    }
    const std::string before = readTestBytes(paths.finalPath);
    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const auto state = db.scopeState();
    CHECK(state.requestedEpoch == epoch);
    CHECK(!state.ready);
    CHECK(state.openState == CatalogDbOpenState::UnsupportedVersion);
    CHECK(state.migration.files == CatalogDbMigrationFileState::FinalOnly);
    CHECK(state.migration.decision == CatalogDbMigrationDecision::FinalDatabaseWins);
    CHECK(readTestBytes(paths.finalPath) == before);
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbFreshBootstrapPathError()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-fresh-path-error-" + suffix + ".example";
    const std::string user = "fresh-path-error-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CHECK(makeCatalogMigrationTestDirectory("cache/library"));
    writeCatalogMigrationFile(paths.libraryDirectory, "not a directory");

    CatalogDb db;
    const auto epoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const auto state = db.scopeState();
    CHECK(state.requestedEpoch == epoch);
    CHECK(!state.ready);
    CHECK(state.status == CatalogDbScopeStatus::OpenFailed);
    CHECK(state.error == CatalogDbErrorCategory::CorruptOrIo);
    CHECK(state.openState == CatalogDbOpenState::CorruptOrIo);
    CHECK(state.migration.files == CatalogDbMigrationFileState::PathError);
    CHECK(state.migration.decision == CatalogDbMigrationDecision::PathError);
    removeCatalogMigrationTestPaths(paths);
}

static bool readHierarchyCheckpoint(const std::string &path,
                                    const std::string &seriesId,
                                    bool &complete, std::int64_t &generation)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr)
            != SQLITE_OK
        || !db) {
        if (db) {
            sqlite3_close(db);
        }
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        db,
        "SELECT complete, last_generation FROM hierarchy_state "
        "WHERE series_id=?1",
        -1, &statement, nullptr) == SQLITE_OK;
    const int bindRc = prepared
        ? sqlite3_bind_text(statement, 1, seriesId.c_str(), -1,
                            SQLITE_TRANSIENT)
        : SQLITE_ERROR;
    const int rc = bindRc == SQLITE_OK ? sqlite3_step(statement) : bindRc;
    if (prepared && rc == SQLITE_ROW) {
        complete = sqlite3_column_int(statement, 0) != 0;
        generation = sqlite3_column_int64(statement, 1);
    }
    if (statement) {
        sqlite3_finalize(statement);
    }
    sqlite3_close(db);
    return prepared && rc == SQLITE_ROW;
}

static void testCatalogDbJellyfinHierarchyStaging()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-jellyfin-staging-" + suffix + ".example";
    const std::string user = "jellyfin-staging-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series;
    series.id = "__task15_series__";
    series.type = "show";
    series.title = "Synthetic Series";
    MediaItem season;
    season.id = "__task15_season__";
    season.type = "season";
    season.seriesId = series.id;
    season.title = "Synthetic Season";
    MediaItem episode;
    episode.id = "__task15_episode__";
    episode.type = "episode";
    episode.seriesId = series.id;
    episode.seasonId = season.id;
    episode.title = "Synthetic Episode";
    const std::map<std::string, std::vector<MediaItem>> completeEpisodes = {
        {season.id, {episode}}};
    {
        CatalogDb db;
        CHECK(db.configureScope(url, user) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        auto complete = db.stageSeriesHierarchy(
            series, {season}, completeEpisodes, 41, 1000, true).get();
        CHECK(complete.success && complete.workerOwned);
        bool checkpointComplete = false;
        std::int64_t generation = 0;
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      checkpointComplete, generation));
        CHECK(checkpointComplete && generation == 41);

        auto partial = db.stageSeriesHierarchy(
            series, {season}, {{season.id, {}}}, 42, 1100, false).get();
        CHECK(partial.success && partial.workerOwned);
        auto retained = db.getEpisodes(season.id).get();
        CHECK(retained.success && retained.items.size() == 1
              && retained.items[0].id == episode.id);
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      checkpointComplete, generation));
        CHECK(!checkpointComplete && generation == 42);

        auto repeated = db.stageSeriesHierarchy(
            series, {season}, completeEpisodes, 43, 1200, true).get();
        CHECK(repeated.success && repeated.workerOwned);
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      checkpointComplete, generation));
        CHECK(checkpointComplete && generation == 43);
    }
    removeCatalogMigrationTestPaths(paths);
}

static void testHomeCatalogHierarchyIntegration()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-home-catalog-integration-" + suffix + ".example";
    const std::string user = "home-catalog-integration-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    MediaItem series;
    series.id = "__task17_home_series__";
    series.type = "show";
    series.title = "Authoritative Series";
    MediaItem season;
    season.id = "__task17_home_season__";
    season.type = "season";
    season.seriesId = series.id;
    season.title = "Authoritative Season";
    MediaItem episode;
    episode.id = "__task17_home_episode__";
    episode.type = "episode";
    episode.seriesId = series.id;
    episode.seasonId = season.id;
    episode.title = "Authoritative Episode";
    const std::map<std::string, std::vector<MediaItem>> episodes = {
        {season.id, {episode}}};

    auto db = std::make_shared<CatalogDb>();
    const auto epoch = db->configureScope(url, user);
    CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db->scopeState().ready);

    auto initialSync=db->readSyncState(false,0,0).get();
    CHECK(initialSync.success && !initialSync.migrated
          && initialSync.lastSuccessfulMs==0
          && initialSync.lastReconcileMs==0
          && initialSync.committedGeneration==0);
    auto seededSync=db->readSyncState(true,1111,777).get();
    CHECK(seededSync.success && seededSync.migrated
          && seededSync.lastSuccessfulMs==1111
          && seededSync.lastReconcileMs==777);
    auto repeatedSeed=db->readSyncState(true,2222,888).get();
    CHECK(repeatedSeed.success && !repeatedSeed.migrated
          && repeatedSeed.lastSuccessfulMs==1111
          && repeatedSeed.lastReconcileMs==777);

    Session session;
    session.serverUrl = url;
    session.accessToken = "synthetic-token";
    session.userId = user;
    {
        HomeScreen home(session, {}, db, epoch);

        auto online = home.submitCatalogHierarchyForTest(
            series, {season}, episodes, 1, true).get();
        CHECK(online.success && online.workerOwned);
        auto afterSeries=db->readSyncState(false,0,0).get();
        CHECK(afterSeries.success && afterSeries.lastSuccessfulMs==1111
              && afterSeries.lastReconcileMs==777
              && afterSeries.committedGeneration==0);
        bool complete = false;
        std::int64_t generation = 0;
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      complete, generation));
        CHECK(complete && generation == 1);

        MediaItem invalidSeries = series;
        invalidSeries.type = "movie";
        auto failed = home.submitCatalogHierarchyForTest(
            invalidSeries, {season}, episodes, 2, true).get();
        CHECK(!failed.success && !failed.cancelled && !failed.superseded);
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      complete, generation));
        CHECK(complete && generation == 1);

        MediaItem offlineEpisode = episode;
        offlineEpisode.title = "Synthetic Offline Episode";
        CHECK(db->stageSeriesHierarchy(
                  series, {season}, {{season.id, {offlineEpisode}}},
                  2, 2000, false).get().success);
        auto authoritative = home.submitCatalogHierarchyForTest(
            series, {season}, episodes, 3, true).get();
        CHECK(authoritative.success);
        auto reconciled = db->getEpisodes(season.id).get();
        CHECK(reconciled.success && reconciled.items.size() == 1
              && reconciled.items[0].title == episode.title);

        auto repeated = home.submitCatalogHierarchyForTest(
            series, {season}, episodes, 4, true).get();
        CHECK(repeated.success);
        auto repeatedRead = db->getEpisodes(season.id).get();
        CHECK(repeatedRead.success && repeatedRead.items.size() == 1);

        auto incomplete = home.submitCatalogHierarchyForTest(
            series, {season}, {{season.id, {}}}, 5, false).get();
        CHECK(!incomplete.success && !incomplete.cancelled
              && !incomplete.superseded);
        CHECK(readHierarchyCheckpoint(paths.finalPath, series.id,
                                      complete, generation));
        CHECK(complete && generation == 4);

        auto checkpoint=db->writeSyncState(4000,3500,4).get();
        CHECK(checkpoint.success && checkpoint.workerOwned
              && checkpoint.committedGeneration==4);
        auto checkpointRead=db->readSyncState(false,0,0).get();
        CHECK(checkpointRead.success && checkpointRead.lastSuccessfulMs==4000
              && checkpointRead.lastReconcileMs==3500
              && checkpointRead.committedGeneration==4);

        const std::string otherUrl =
            "https://sqlite-home-catalog-other-" + suffix + ".example";
        const std::string otherUser = "home-catalog-other-user";
        const auto otherEpoch = db->configureScope(otherUrl, otherUser);
        CHECK(otherEpoch != epoch);
        CHECK(db->waitForIdleForTest(std::chrono::seconds(2)));
        CatalogDbJobMetadata staleMetadata;
        staleMetadata.scopeEpoch=epoch;
        auto staleCheckpoint=db->writeSyncState(5000,4500,6,staleMetadata).get();
        CHECK(!staleCheckpoint.success && staleCheckpoint.superseded);
        auto stale = home.submitCatalogHierarchyForTest(
            series, {season}, episodes, 6, true).get();
        CHECK(!stale.success && stale.superseded);
        auto otherRead = db->getSeasons(series.id).get();
        CHECK(otherRead.success && otherRead.items.empty());
        removeCatalogMigrationTestPaths(
            catalogMigrationTestPaths(otherUrl, otherUser));
    }
    removeCatalogMigrationTestPaths(paths);
}

static bool readOfflineMovie(const std::string &path, const std::string &id,
                             std::string &title, std::int64_t &position)
{
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr)
            != SQLITE_OK
        || !db) {
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_stmt *statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        db, "SELECT title, playback_position_ticks FROM media_items "
            "WHERE id=?1 AND kind=1", -1, &statement, nullptr) == SQLITE_OK;
    const int bindRc = prepared
        ? sqlite3_bind_text(statement, 1, id.c_str(), -1, SQLITE_TRANSIENT)
        : SQLITE_ERROR;
    const int rc = bindRc == SQLITE_OK ? sqlite3_step(statement) : bindRc;
    if (rc == SQLITE_ROW) {
        const unsigned char *value = sqlite3_column_text(statement, 0);
        title = value ? reinterpret_cast<const char *>(value) : "";
        position = sqlite3_column_int64(statement, 1);
    }
    if (statement) sqlite3_finalize(statement);
    sqlite3_close(db);
    return prepared && rc == SQLITE_ROW;
}

static void testCatalogDbOfflineDownloadReconstruction()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-offline-downloads-" + suffix + ".example";
    const std::string user = "offline-downloads-user";
    const std::string root = "downloads/task16-" + suffix;
    DownloadStore store(root);
    const std::string scope = store.scopeKey(url, user);
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    DownloadItem episode;
    episode.itemId = "__task16_downloaded_episode__";
    episode.itemType = "episode";
    episode.title = "Downloaded Episode";
    episode.seriesId = "__task16_downloaded_series__";
    episode.seriesName = "Downloaded Series";
    episode.seasonId = "__task16_downloaded_season__";
    episode.seasonName = "Downloaded Season";
    episode.seasonNumber = 3;
    episode.episodeNumber = 7;
    episode.runtimeTicks = 1000;
    episode.playbackPositionTicks = 250;
    episode.expectedSize = 1;
    episode.state = DownloadState::Complete;
    DownloadItem movie = episode;
    movie.itemId = "__task16_downloaded_movie__";
    movie.itemType = "movie";
    movie.title = "Downloaded Movie";
    movie.seriesId.clear();
    movie.seriesName.clear();
    movie.seasonId.clear();
    movie.seasonName.clear();
    movie.seasonNumber = 0;
    movie.episodeNumber = 0;
    movie.playbackPositionTicks = 400;
    DownloadItem missing = episode;
    missing.itemId = "__task16_missing_parent_episode__";
    missing.title = "Unparented Download";
    missing.seriesId.clear();
    missing.seriesName.clear();
    missing.seasonId.clear();
    missing.seasonName.clear();
    const std::vector<DownloadItem> fixtures = {episode, movie, missing};
    for (const auto &item : fixtures) {
        CHECK(store.saveManifest(scope, item));
    }
    CHECK(store.saveIndex(scope, fixtures));
    const std::string episodeBefore =
        readTestBytes(store.manifestPath(scope, episode.itemId));
    const std::string movieBefore =
        readTestBytes(store.manifestPath(scope, movie.itemId));

    {
        CatalogDb db;
        CHECK(db.configureScope(url, user) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        auto rebuilt = db.reconstructOfflineDownloads(root).get();
        CHECK(rebuilt.success && rebuilt.workerOwned && !rebuilt.skipped);
        CHECK(rebuilt.itemsUpserted == 4 && rebuilt.containersSynthesized == 2);
        const auto seasons = db.getSeasons(episode.seriesId).get();
        const auto episodes = db.getEpisodes(episode.seasonId).get();
        CHECK(seasons.success && seasons.items.size() == 1
              && seasons.items[0].id == episode.seasonId);
        CHECK(episodes.success && episodes.items.size() == 1
              && episodes.items[0].id == episode.itemId
              && episodes.items[0].seriesName == episode.seriesName
              && episodes.items[0].playbackPositionTicks == 250
              && episodes.items[0].indexNumber == 7);
        bool checkpointComplete = true;
        std::int64_t generation = -1;
        CHECK(readHierarchyCheckpoint(paths.finalPath, episode.seriesId,
                                      checkpointComplete, generation));
        CHECK(!checkpointComplete);
        auto repeated = db.reconstructOfflineDownloads(root).get();
        CHECK(repeated.success && repeated.itemsUpserted == 4);
        std::string movieTitle;
        std::int64_t moviePosition = -1;
        CHECK(readOfflineMovie(paths.finalPath, movie.itemId, movieTitle,
                               moviePosition));
        CHECK(movieTitle == movie.title && moviePosition == 400);
        CHECK(db.getEpisodes(missing.seasonId).get().items.empty());
    }
    std::vector<DownloadItem> loaded;
    CHECK(store.loadCompleteMetadata(scope, loaded) && loaded.size() == 3);
    CHECK(readTestBytes(store.manifestPath(scope, episode.itemId))
          == episodeBefore);
    CHECK(readTestBytes(store.manifestPath(scope, movie.itemId)) == movieBefore);
    for (const auto &item : fixtures) {
        store.removeItem(scope, item.itemId);
    }
    std::remove((store.scopePath(scope) + "/index.v1").c_str());
    ::rmdir((store.scopePath(scope) + "/items").c_str());
    ::rmdir(store.scopePath(scope).c_str());
    ::rmdir(root.c_str());
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbScopeActivationSchedulesOfflineRebuild()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url =
        "https://sqlite-scope-activation-" + suffix + ".example";
    const std::string user = "scope-activation-user";
    const std::string root = "downloads";
    DownloadStore store(root);
    const std::string scope = store.scopeKey(url, user);
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    DownloadItem episode;
    episode.itemId = "__task17_activation_episode__";
    episode.itemType = "episode";
    episode.title = "Activation Episode";
    episode.seriesId = "__task17_activation_series__";
    episode.seriesName = "Activation Series";
    episode.seasonId = "__task17_activation_season__";
    episode.seasonName = "Activation Season";
    episode.seasonNumber = 1;
    episode.episodeNumber = 2;
    episode.runtimeTicks = 1000;
    episode.expectedSize = 1;
    episode.state = DownloadState::Complete;
    CHECK(store.saveManifest(scope, episode));
    CHECK(store.saveIndex(scope, {episode}));

    {
        CatalogDb db;
        CHECK(db.configureScope(url, user) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.ready);
        const auto seasons = db.getSeasons(episode.seriesId).get();
        const auto episodes = db.getEpisodes(episode.seasonId).get();
        CHECK(seasons.success && seasons.items.size() == 1
              && seasons.items[0].id == episode.seasonId);
        CHECK(episodes.success && episodes.items.size() == 1
              && episodes.items[0].id == episode.itemId);
    }

    store.removeItem(scope, episode.itemId);
    std::remove((store.scopePath(scope) + "/index.v1").c_str());
    ::rmdir((store.scopePath(scope) + "/items").c_str());
    ::rmdir(store.scopePath(scope).c_str());
    removeCatalogMigrationTestPaths(paths);
}
