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
            CHECK(state.ready);
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
