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

static bool readHierarchyCheckpoint(const std::string &path, bool &complete,
                                    std::int64_t &generation)
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
        "WHERE series_id='__task15_series__'",
        -1, &statement, nullptr) == SQLITE_OK;
    const int rc = prepared ? sqlite3_step(statement) : SQLITE_ERROR;
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
        CHECK(readHierarchyCheckpoint(paths.finalPath, checkpointComplete,
                                      generation));
        CHECK(checkpointComplete && generation == 41);

        auto partial = db.stageSeriesHierarchy(
            series, {season}, {{season.id, {}}}, 42, 1100, false).get();
        CHECK(partial.success && partial.workerOwned);
        auto retained = db.getEpisodes(season.id).get();
        CHECK(retained.success && retained.items.size() == 1
              && retained.items[0].id == episode.id);
        CHECK(readHierarchyCheckpoint(paths.finalPath, checkpointComplete,
                                      generation));
        CHECK(!checkpointComplete && generation == 42);

        auto repeated = db.stageSeriesHierarchy(
            series, {season}, completeEpisodes, 43, 1200, true).get();
        CHECK(repeated.success && repeated.workerOwned);
        CHECK(readHierarchyCheckpoint(paths.finalPath, checkpointComplete,
                                      generation));
        CHECK(checkpointComplete && generation == 43);
    }
    removeCatalogMigrationTestPaths(paths);
}
