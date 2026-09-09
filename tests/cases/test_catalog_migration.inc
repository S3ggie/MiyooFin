#include <atomic>
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <thread>
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
    std::remove(paths.finalPath.c_str());
    std::remove(paths.migratingPath.c_str());
    std::remove(paths.legacyPath.c_str());
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

bool seedCatalogMigrationFinalFile(const std::string &path)
{
    sqlite3 *db = nullptr;
    const int rc = sqlite3_open_v2(
        path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (db) {
        sqlite3_close(db);
    }
    return rc == SQLITE_OK;
}

void writeCatalogMigrationFile(const std::string &path, const std::string &value)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

} // namespace

static void testCatalogDbMigrationPathStates()
{
    struct ExpectedState {
        bool legacy;
        bool final;
        bool migrating;
        CatalogDbMigrationFileState files;
        CatalogDbMigrationDecision decision;
        bool finalWins;
        bool cleanupCandidate;
    };
    const ExpectedState expected[] = {
        {false, false, false, CatalogDbMigrationFileState::NoFiles,
         CatalogDbMigrationDecision::CreateEmptyFinal, false, false},
        {true, false, false, CatalogDbMigrationFileState::LegacyOnly,
         CatalogDbMigrationDecision::StageLegacyImport, false, false},
        {false, true, false, CatalogDbMigrationFileState::FinalOnly,
         CatalogDbMigrationDecision::FinalDatabaseWins, true, false},
        {false, false, true, CatalogDbMigrationFileState::MigratingOnly,
         CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart, false,
         false},
        {true, true, false, CatalogDbMigrationFileState::LegacyAndFinal,
         CatalogDbMigrationDecision::FinalDatabaseWins, true, false},
        {true, false, true, CatalogDbMigrationFileState::LegacyAndMigrating,
         CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart, false,
         false},
        {false, true, true, CatalogDbMigrationFileState::FinalAndMigrating,
         CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate, true,
         true},
        {true, true, true,
         CatalogDbMigrationFileState::LegacyFinalAndMigrating,
         CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate, true,
         true},
    };

    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    for (std::size_t index = 0; index < sizeof(expected) / sizeof(expected[0]);
         ++index) {
        const std::string url = "https://sqlite-migration-state-"
            + suffix + "-" + std::to_string(index) + ".example";
        const std::string user = "migration-state-user-" + std::to_string(index);
        const auto paths = catalogMigrationTestPaths(url, user);
        removeCatalogMigrationTestPaths(paths);

        if (expected[index].legacy) {
            OfflineCatalogSnapshot snapshot;
            CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
        }
        if (expected[index].migrating) {
            CHECK(makeCatalogMigrationTestDirectory(paths.libraryDirectory));
            writeCatalogMigrationFile(paths.migratingPath, "stale-import");
        }
        if (expected[index].final) {
            CHECK(makeCatalogMigrationTestDirectory(paths.libraryDirectory));
            CHECK(seedCatalogMigrationFinalFile(paths.finalPath));
        }

        const std::string legacyBefore = readTestBytes(paths.legacyPath);
        const std::string migratingBefore = readTestBytes(paths.migratingPath);
        {
            CatalogDb db;
            db.setLegacyMigrationAutoActivationForTest(false);
            const auto epoch = db.configureScope(url, user);
            CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
            const auto state = db.scopeState();
            CHECK(state.requestedEpoch == epoch);
            CHECK(state.migration.legacyPresent == expected[index].legacy);
            CHECK(state.migration.finalPresent == expected[index].final);
            CHECK(state.migration.migratingPresent == expected[index].migrating);
            CHECK(state.migration.files == expected[index].files);
            CHECK(state.migration.decision == expected[index].decision);
            CHECK(state.migration.finalWins == expected[index].finalWins);
            CHECK(state.migration.migratingCleanupCandidate
                  == expected[index].cleanupCandidate);
            CHECK(!state.migration.pathError);
            CHECK(state.ready == (expected[index].final
                                  || (!expected[index].legacy
                                      && !expected[index].migrating)));
        }
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
        CHECK(readTestBytes(paths.migratingPath) == migratingBefore);
        removeCatalogMigrationTestPaths(paths);
    }
}

static void testCatalogDbMigrationFutureFinalPreserved()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-migration-future-" + suffix
        + ".example";
    const std::string user = "migration-future-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);

    CatalogDb db;
    const auto createdEpoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    CHECK(db.scopeState().requestedEpoch == createdEpoch
          && db.scopeState().ready);
    CHECK(db.setSchemaMetadataForTest(0x4D59464E, 99).success);
    const std::string before = readTestBytes(paths.finalPath);
    CHECK(db.deconfigureScope() > createdEpoch);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));

    const auto rejectedEpoch = db.configureScope(url, user);
    CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    const auto state = db.scopeState();
    CHECK(state.requestedEpoch == rejectedEpoch);
    CHECK(!state.ready);
    CHECK(state.openState == CatalogDbOpenState::UnsupportedVersion);
    CHECK(state.migration.files == CatalogDbMigrationFileState::FinalOnly);
    CHECK(state.migration.decision == CatalogDbMigrationDecision::FinalDatabaseWins);
    CHECK(readTestBytes(paths.finalPath) == before);
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbMigrationPathError()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-migration-path-error-" + suffix
        + ".example";
    const std::string user = "migration-path-error-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    CHECK(makeCatalogMigrationTestDirectory("cache/library"));
    std::ofstream directoryPath(paths.libraryDirectory,
                                std::ios::binary | std::ios::trunc);
    directoryPath << "not a directory";
    directoryPath.close();

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
    CHECK(state.migration.pathError);
    removeCatalogMigrationTestPaths(paths);
}

OfflineCatalogSnapshot makeCatalogMigrationSnapshot()
{
    OfflineCatalogSnapshot snapshot;
    MediaItem series;
    series.id = "__task15_series__";
    series.type = "show";
    series.title = "München 世界";
    series.overview = "A complete migration fixture";
    series.year = 2025;
    series.rating = 8.5f;
    series.genre = "Drama";
    series.genres = {"Drama", "Science Fiction"};
    series.etag = "task15-series-etag";
    series.played = true;
    series.progress = 0.625f;
    series.playbackPositionTicks = 1234567;
    series.imageTags["Primary"] = "series-image-tag";
    series.artR = 10;
    series.artG = 20;
    series.artB = 30;

    MediaItem seasonOne;
    seasonOne.id = "__task15_season_one__";
    seasonOne.type = "season";
    seasonOne.title = "Season One";
    seasonOne.seriesId = series.id;
    seasonOne.indexNumber = 1;
    seasonOne.imageTags["Primary"] = "season-one-tag";

    MediaItem seasonTwo = seasonOne;
    seasonTwo.id = "__task15_season_two__";
    seasonTwo.title = "Season Two";
    seasonTwo.indexNumber = 2;

    MediaItem episode;
    episode.id = "__task15_episode__";
    episode.type = "episode";
    episode.title = "Episode 世界";
    episode.seriesId = series.id;
    episode.seasonId = seasonOne.id;
    episode.indexNumber = 3;
    episode.parentIndexNumber = 1;
    episode.runTimeTicks = 9876543;
    episode.playbackPositionTicks = 3456789;
    episode.progress = 0.35f;
    episode.genre = "Mystery";
    episode.genres = {"Mystery"};
    episode.imageTags["Primary"] = "episode-tag";

    snapshot.series.emplace(series.id, series);
    snapshot.seasonsBySeries[series.id] = {seasonOne, seasonTwo};
    snapshot.episodesBySeason[seasonOne.id] = {episode};
    snapshot.episodesBySeason[seasonTwo.id] = {};
    return snapshot;
}

static void testCatalogDbLegacyMigration()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-legacy-import-" + suffix
        + ".example";
    const std::string user = "legacy-import-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    const OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
    CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
    const std::string legacyBefore = readTestBytes(paths.legacyPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(url, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(state.migration.attempted && state.migration.succeeded);
        const auto schema = db.runSchemaDiagnosticsForTest();
        CHECK(schema.success && schema.applicationId == 0x4D59464E
              && schema.userVersion == 1);
        const auto seasons = db.getSeasons("__task15_series__").get();
        CHECK(seasons.success && seasons.items.size() == 2);
        if (seasons.items.size() == 2) {
            CHECK(seasons.items[0].id == "__task15_season_one__");
            CHECK(seasons.items[1].id == "__task15_season_two__");
            CHECK(seasons.items[0].imageTags.at("Primary") == "season-one-tag");
        }
        const auto episodes = db.getEpisodes("__task15_season_one__").get();
        CHECK(episodes.success && episodes.items.size() == 1);
        if (episodes.items.size() == 1) {
            CHECK(episodes.items[0].title == "Episode 世界");
            CHECK(episodes.items[0].playbackPositionTicks == 3456789);
            CHECK(episodes.items[0].genres == std::vector<std::string>{"Mystery"});
            CHECK(episodes.items[0].imageTags.at("Primary") == "episode-tag");
        }
        CHECK(access(paths.migratingPath.c_str(), F_OK) != 0);
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
    }
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbLegacyContainerRelationshipRepair()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-legacy-container-repair-"
        + suffix + ".example";
    const std::string user = "legacy-container-repair-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
    for (auto &entry : snapshot.seasonsBySeries) {
        for (auto &season : entry.second) {
            season.seriesId.clear();
        }
    }
    for (auto &entry : snapshot.episodesBySeason) {
        for (auto &episode : entry.second) {
            episode.seasonId = "legacy-stale-parent";
        }
    }
    CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
    const std::string legacyBefore = readTestBytes(paths.legacyPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(url, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(state.migration.attempted && state.migration.succeeded);
        const auto seasons = db.getSeasons("__task15_series__").get();
        CHECK(seasons.success && seasons.items.size() == 2);
        if (seasons.items.size() == 2) {
            CHECK(seasons.items[0].seriesId == "__task15_series__");
            CHECK(seasons.items[1].seriesId == "__task15_series__");
        }
        const auto episodes = db.getEpisodes("__task15_season_one__").get();
        CHECK(episodes.success && episodes.items.size() == 1);
        if (episodes.items.size() == 1) {
            CHECK(episodes.items[0].seasonId == "__task15_season_one__");
        }
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
    }
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbLegacyDuplicateEpisodes()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));

    {
        const std::string url = "https://sqlite-legacy-duplicate-same-"
            + suffix + ".example";
        const std::string user = "legacy-duplicate-same-user";
        const auto paths = catalogMigrationTestPaths(url, user);
        removeCatalogMigrationTestPaths(paths);
        OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
        const MediaItem episode = snapshot.episodesBySeason.at(
            "__task15_season_one__").front();
        snapshot.episodesBySeason.at("__task15_season_one__").push_back(
            episode);
        MediaItem duplicate = episode;
        duplicate.seriesId = "stale-series";
        duplicate.seasonId = "stale-season";
        snapshot.episodesBySeason.at("__task15_season_one__").push_back(
            duplicate);
        CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
        const std::string legacyBefore = readTestBytes(paths.legacyPath);
        {
            CatalogDb db;
            const auto epoch = db.configureScope(url, user);
            CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
            const auto state = db.scopeState();
            CHECK(state.requestedEpoch == epoch && state.ready);
            CHECK(state.migration.attempted && state.migration.succeeded);
            const auto episodes = db.getEpisodes(
                "__task15_season_one__").get();
            CHECK(episodes.success && episodes.items.size() == 1);
            if (episodes.items.size() == 1) {
                CHECK(episodes.items.front().seriesId == "__task15_series__");
                CHECK(episodes.items.front().seasonId
                      == "__task15_season_one__");
            }
        }
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
        removeCatalogMigrationTestPaths(paths);
    }

    {
        const std::string url = "https://sqlite-legacy-duplicate-stale-"
            + suffix + ".example";
        const std::string user = "legacy-duplicate-stale-user";
        const auto paths = catalogMigrationTestPaths(url, user);
        removeCatalogMigrationTestPaths(paths);
        OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
        const MediaItem episode = snapshot.episodesBySeason.at(
            "__task15_season_one__").front();
        MediaItem stale = episode;
        stale.progress = 0.1f;
        stale.playbackPositionTicks = 1;
        snapshot.episodesBySeason.at("__task15_season_two__").push_back(
            stale);
        CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
        const std::string legacyBefore = readTestBytes(paths.legacyPath);
        {
            CatalogDb db;
            const auto epoch = db.configureScope(url, user);
            CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
            const auto state = db.scopeState();
            CHECK(state.requestedEpoch == epoch && state.ready);
            CHECK(state.migration.attempted && state.migration.succeeded);
            const auto firstSeason = db.getEpisodes(
                "__task15_season_one__").get();
            const auto staleSeason = db.getEpisodes(
                "__task15_season_two__").get();
            CHECK(firstSeason.success && firstSeason.items.size() == 1);
            CHECK(staleSeason.success && staleSeason.items.empty());
            if (firstSeason.items.size() == 1) {
                CHECK(firstSeason.items.front().playbackPositionTicks
                      == episode.playbackPositionTicks);
            }
        }
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
        removeCatalogMigrationTestPaths(paths);
    }

    {
        const std::string url = "https://sqlite-legacy-duplicate-conflict-"
            + suffix + ".example";
        const std::string user = "legacy-duplicate-conflict-user";
        const auto paths = catalogMigrationTestPaths(url, user);
        removeCatalogMigrationTestPaths(paths);
        OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
        MediaItem duplicate = snapshot.episodesBySeason.at(
            "__task15_season_one__").front();
        duplicate.seasonId = "__task15_season_two__";
        snapshot.episodesBySeason.at("__task15_season_two__").push_back(
            duplicate);
        CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
        const std::string legacyBefore = readTestBytes(paths.legacyPath);
        {
            CatalogDb db;
            db.setLegacyMigrationAutoActivationForTest(false);
            const auto epoch = db.configureScope(url, user);
            CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
            CHECK(db.scopeState().requestedEpoch == epoch);
            const auto result = db.runLegacyMigrationForTest();
            CHECK(!result.success);
            CHECK(result.message
                  == "legacy catalog contains duplicate media ID with conflicting hierarchy parents");
            CHECK(access(paths.migratingPath.c_str(), F_OK) != 0);
        }
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
        removeCatalogMigrationTestPaths(paths);
    }
}

static void testOfflineCatalogMigrationGuardBlocksWriters()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const auto paths = catalogMigrationTestPaths(
        "https://legacy-migration-guard-" + suffix + ".example",
        "legacy-migration-guard-user");
    removeCatalogMigrationTestPaths(paths);
    const auto snapshot = makeCatalogMigrationSnapshot();
    CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));

    std::atomic<bool> writerStarted(false);
    std::atomic<bool> writerFinished(false);
    std::thread writer;
    {
        OfflineCatalog::MigrationGuard guard;
        writer = std::thread([&] {
            writerStarted.store(true);
            OfflineCatalog::save(paths.legacyPath, snapshot);
            writerFinished.store(true);
        });
        for (int i = 0; i < 100 && !writerStarted.load(); ++i) {
            std::this_thread::yield();
        }
        CHECK(writerStarted.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!writerFinished.load());
    }
    writer.join();
    CHECK(writerFinished.load());
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbMigrationGuardCoordinatesActualImport()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const auto paths = catalogMigrationTestPaths(
        "https://legacy-migration-actual-guard-" + suffix + ".example",
        "legacy-migration-actual-guard-user");
    removeCatalogMigrationTestPaths(paths);
    const auto snapshot = makeCatalogMigrationSnapshot();
    CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
    const std::string legacyBefore = readTestBytes(paths.legacyPath);
    {
        CatalogDb db;
        db.setLegacyMigrationAutoActivationForTest(false);
        db.setLegacyMigrationPauseForTest(true);
        CHECK(db.configureScope(
                  "https://legacy-migration-actual-guard-" + suffix
                      + ".example",
                  "legacy-migration-actual-guard-user") != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));

        CatalogDbTestResult migration;
        std::atomic<bool> migrationFinished(false);
        std::thread migrationThread([&] {
            migration = db.runLegacyMigrationForTest();
            migrationFinished.store(true);
        });
        bool guardObserved = false;
        for (int i = 0; i < 1000 && !guardObserved; ++i) {
            guardObserved = OfflineCatalog::MigrationGuard::activeForTest();
            if (!guardObserved) {
                std::this_thread::yield();
            }
        }
        CHECK(guardObserved);

        std::atomic<bool> writerStarted(false);
        std::atomic<bool> writerFinished(false);
        std::thread writer([&] {
            writerStarted.store(true);
            OfflineCatalog::save(paths.legacyPath, snapshot);
            writerFinished.store(true);
        });
        for (int i = 0; i < 1000 && !writerStarted.load(); ++i) {
            std::this_thread::yield();
        }
        CHECK(writerStarted.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(!writerFinished.load());

        db.setLegacyMigrationPauseForTest(false);
        migrationThread.join();
        writer.join();
        CHECK(migrationFinished.load() && migration.success);
        CHECK(writerFinished.load());
    }
    CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
    removeCatalogMigrationTestPaths(paths);
}

static void testCatalogDbLegacyMigrationFailures()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const auto runFailure = [&](int index, int failAfterRows,
                                bool failValidation, bool corruptSource,
                                bool staleTemp) {
        const std::string url = "https://sqlite-legacy-failure-" + suffix
            + "-" + std::to_string(index) + ".example";
        const std::string user = "legacy-failure-user-" + std::to_string(index);
        const auto paths = catalogMigrationTestPaths(url, user);
        removeCatalogMigrationTestPaths(paths);
        if (corruptSource) {
            CHECK(makeCatalogMigrationTestDirectory(paths.offlineDirectory));
            writeCatalogMigrationFile(paths.legacyPath, "truncated legacy");
        } else {
            CHECK(OfflineCatalog::save(paths.legacyPath,
                                       makeCatalogMigrationSnapshot()));
        }
        const std::string legacyBefore = readTestBytes(paths.legacyPath);
        if (staleTemp) {
            CHECK(makeCatalogMigrationTestDirectory(paths.libraryDirectory));
            writeCatalogMigrationFile(paths.migratingPath, "interrupted temp");
        }
        {
            CatalogDb db;
            db.setLegacyMigrationFailureForTest(failAfterRows, failValidation);
            const auto epoch = db.configureScope(url, user);
            CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
            const auto state = db.scopeState();
            CHECK(state.requestedEpoch == epoch);
            CHECK(!state.ready && state.migration.attempted
                  && !state.migration.succeeded);
            CHECK(access(paths.finalPath.c_str(), F_OK) != 0);
            CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
            if (staleTemp) {
                CHECK(readTestBytes(paths.migratingPath) != "");
            }
        }
        removeCatalogMigrationTestPaths(paths);
    };

    runFailure(0, -1, false, true, false);
    runFailure(1, 1, false, false, false);
    runFailure(2, -1, true, false, false);

    const std::string interruptedUrl = "https://sqlite-legacy-interrupted-"
        + suffix + ".example";
    const std::string interruptedUser = "legacy-interrupted-user";
    const auto interrupted =
        catalogMigrationTestPaths(interruptedUrl, interruptedUser);
    removeCatalogMigrationTestPaths(interrupted);
    CHECK(OfflineCatalog::save(interrupted.legacyPath,
                               makeCatalogMigrationSnapshot()));
    CHECK(makeCatalogMigrationTestDirectory(interrupted.libraryDirectory));
    writeCatalogMigrationFile(interrupted.migratingPath, "interrupted temp");
    {
        CatalogDb db;
        CHECK(db.configureScope(interruptedUrl, interruptedUser) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.ready && state.migration.attempted
              && state.migration.succeeded);
        CHECK(access(interrupted.migratingPath.c_str(), F_OK) != 0);
    }
    removeCatalogMigrationTestPaths(interrupted);
}

static void testCatalogDbAutomaticScopeActivation()
{
    const std::string suffix =
        std::to_string(static_cast<long long>(getpid()));
    const std::string url = "https://sqlite-auto-activation-" + suffix
        + ".example";
    const std::string user = "automatic-activation-user";
    const auto paths = catalogMigrationTestPaths(url, user);
    removeCatalogMigrationTestPaths(paths);
    const OfflineCatalogSnapshot snapshot = makeCatalogMigrationSnapshot();
    CHECK(OfflineCatalog::save(paths.legacyPath, snapshot));
    const std::string legacyBefore = readTestBytes(paths.legacyPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(url, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(state.migration.attempted && state.migration.succeeded);
        CHECK(state.migration.finalPresent && state.migration.legacyPresent);
        CHECK(state.migration.finalWins);
        CHECK(readTestBytes(paths.legacyPath) == legacyBefore);
    }
    removeCatalogMigrationTestPaths(paths);

    const std::string freshUrl = "https://sqlite-auto-fresh-" + suffix
        + ".example";
    const auto freshPaths = catalogMigrationTestPaths(freshUrl, user);
    removeCatalogMigrationTestPaths(freshPaths);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(freshUrl, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(!state.migration.attempted && !state.migration.succeeded);
        CHECK(access(freshPaths.finalPath.c_str(), F_OK) == 0
              && !state.migration.legacyPresent);
    }
    removeCatalogMigrationTestPaths(freshPaths);

    const std::string finalUrl = "https://sqlite-auto-final-" + suffix
        + ".example";
    const auto finalPaths = catalogMigrationTestPaths(finalUrl, user);
    removeCatalogMigrationTestPaths(finalPaths);
    {
        CatalogDb db;
        CHECK(db.configureScope(finalUrl, user) != 0);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
    }
    writeCatalogMigrationFile(finalPaths.legacyPath, "must remain legacy");
    const std::string legacyFinalBefore = readTestBytes(finalPaths.legacyPath);
    {
        CatalogDb db;
        const auto epoch = db.configureScope(finalUrl, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && state.ready);
        CHECK(!state.migration.attempted && !state.migration.succeeded
              && state.migration.finalWins);
        CHECK(readTestBytes(finalPaths.legacyPath) == legacyFinalBefore);
    }
    removeCatalogMigrationTestPaths(finalPaths);

    const std::string failedUrl = "https://sqlite-auto-failed-" + suffix
        + ".example";
    const auto failedPaths = catalogMigrationTestPaths(failedUrl, user);
    removeCatalogMigrationTestPaths(failedPaths);
    CHECK(makeCatalogMigrationTestDirectory(failedPaths.offlineDirectory));
    writeCatalogMigrationFile(failedPaths.legacyPath, "invalid legacy source");
    {
        CatalogDb db;
        const auto epoch = db.configureScope(failedUrl, user);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epoch && !state.ready);
        CHECK(state.status == CatalogDbScopeStatus::OpenFailed
              && state.error == CatalogDbErrorCategory::CorruptOrIo);
        CHECK(state.migration.attempted && !state.migration.succeeded);
        CHECK(!state.migration.finalPresent);
    }
    removeCatalogMigrationTestPaths(failedPaths);

    const std::string rapidA = "https://sqlite-auto-rapid-a-" + suffix
        + ".example";
    const std::string rapidB = "https://sqlite-auto-rapid-b-" + suffix
        + ".example";
    const std::string rapidC = "https://sqlite-auto-rapid-c-" + suffix
        + ".example";
    const auto pathsA = catalogMigrationTestPaths(rapidA, user);
    const auto pathsB = catalogMigrationTestPaths(rapidB, user);
    const auto pathsC = catalogMigrationTestPaths(rapidC, user);
    removeCatalogMigrationTestPaths(pathsA);
    removeCatalogMigrationTestPaths(pathsB);
    removeCatalogMigrationTestPaths(pathsC);
    {
        CatalogDb db;
        db.setWorkerPausedForTest(true);
        const auto epochA = db.configureScope(rapidA, user);
        const auto epochB = db.configureScope(rapidB, user);
        const auto epochC = db.configureScope(rapidC, user);
        db.setWorkerPausedForTest(false);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == epochC && state.ready);
        CHECK(!db.canPublishForTest(epochA) && !db.canPublishForTest(epochB)
              && db.canPublishForTest(epochC));
    }
    removeCatalogMigrationTestPaths(pathsA);
    removeCatalogMigrationTestPaths(pathsB);
    removeCatalogMigrationTestPaths(pathsC);

    const std::string logoutUrl = "https://sqlite-auto-logout-" + suffix
        + ".example";
    const auto logoutPaths = catalogMigrationTestPaths(logoutUrl, user);
    removeCatalogMigrationTestPaths(logoutPaths);
    {
        CatalogDb db;
        db.setWorkerPausedForTest(true);
        const auto oldEpoch = db.configureScope(logoutUrl, user);
        const auto clearedEpoch = db.deconfigureScope();
        db.setWorkerPausedForTest(false);
        CHECK(db.waitForIdleForTest(std::chrono::seconds(2)));
        const auto state = db.scopeState();
        CHECK(state.requestedEpoch == clearedEpoch
              && state.status == CatalogDbScopeStatus::Unconfigured
              && !state.ready && !db.canPublishForTest(oldEpoch));
    }
    removeCatalogMigrationTestPaths(logoutPaths);
}
