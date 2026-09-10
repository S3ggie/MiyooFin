#include "catalog_journal_benchmark.hpp"

#include "../src/catalog/CatalogDbSchema.hpp"
#include "../src/catalog/MediaItemSql.hpp"
#include "../src/data/MediaItem.hpp"
#include "../vendor/sqlite/sqlite3.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace miyoofin {

namespace {

namespace fs = std::filesystem;

struct Profile {
    char id;
    const char *journal;
    const char *synchronous;
    const char *locking;
};

constexpr Profile kProfiles[] = {
    {'A', "DELETE", "FULL", "NORMAL"},
    {'B', "DELETE", "EXTRA", "NORMAL"},
    {'C', "WAL", "FULL", "NORMAL"},
    {'D', "WAL", "NORMAL", "NORMAL"},
    {'E', "WAL", "FULL", "EXCLUSIVE"},
    {'F', "WAL", "NORMAL", "EXCLUSIVE"},
};

struct Metrics {
    long long readBytes = 0;
    long long writeBytes = 0;
    long long rssKb = 0;
    long long cpuUs = 0;
};

struct Database {
    sqlite3 *db = nullptr;
    Profile profile{};
    std::string path;

    Database() = default;

    ~Database()
    {
        if (db) {
            sqlite3_close(db);
        }
    }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
};

struct TransactionResult {
    bool success = false;
    long long durationUs = 0;
    long long commitUs = 0;
    std::size_t rows = 0;
    std::string error;
};

struct ArtifactSizes {
    long long database = 0;
    long long journal = 0;
    long long wal = 0;
    long long shm = 0;
};

std::string lowerAscii(const char *value)
{
    std::string result = value ? value : "";
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

const Profile *profileFor(char id)
{
    for (const Profile &profile : kProfiles) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

long long monotonicUs()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

bool execSql(sqlite3 *db, const char *sql, std::string &error)
{
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc == SQLITE_OK) {
        return true;
    }
    error = sqliteError ? sqliteError : sqlite3_errmsg(db);
    sqlite3_free(sqliteError);
    return false;
}

bool scalar(sqlite3 *db, const std::string &sql, std::string &value,
            std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const int step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        value = text ? reinterpret_cast<const char *>(text) : "";
    } else {
        error = sqlite3_errmsg(db);
    }
    const int finalRc = sqlite3_finalize(statement);
    return step == SQLITE_ROW && finalRc == SQLITE_OK;
}

bool scalarInt(sqlite3 *db, const std::string &sql, long long &value,
               std::string &error)
{
    std::string text;
    if (!scalar(db, sql, text, error)) {
        return false;
    }
    value = std::strtoll(text.c_str(), nullptr, 10);
    return true;
}

bool prepareSchema(sqlite3 *db, std::string &error)
{
    if (!execSql(db, "BEGIN IMMEDIATE;", error)) {
        return false;
    }
    for (const char *statement : kCatalogSchemaStatements) {
        if (!execSql(db, statement, error)) {
            std::string ignored;
            execSql(db, "ROLLBACK;", ignored);
            return false;
        }
    }
    const std::string applicationId =
        "PRAGMA application_id = " + std::to_string(kCatalogApplicationId) + ";";
    if (!execSql(db, applicationId.c_str(), error)
        || !execSql(db, "PRAGMA user_version = 1;", error)
        || !execSql(db, "COMMIT;", error)) {
        std::string ignored;
        execSql(db, "ROLLBACK;", ignored);
        return false;
    }
    return true;
}

bool configure(Database &database, std::string &error)
{
    sqlite3 *db = database.db;
    if (!execSql(db, "PRAGMA foreign_keys = ON;", error)
        || !execSql(db, "PRAGMA trusted_schema = OFF;", error)) {
        return false;
    }
    const std::string locking = "PRAGMA locking_mode = "
        + std::string(database.profile.locking) + ";";
    const std::string journal = "PRAGMA journal_mode = "
        + std::string(database.profile.journal) + ";";
    const std::string synchronous = "PRAGMA synchronous = "
        + std::string(database.profile.synchronous) + ";";
    if (!execSql(db, locking.c_str(), error)
        || !execSql(db, journal.c_str(), error)
        || !execSql(db, synchronous.c_str(), error)) {
        return false;
    }
    std::string activeJournal;
    std::string activeLocking;
    std::string activeSynchronous;
    if (!scalar(db, "PRAGMA journal_mode;", activeJournal, error)
        || !scalar(db, "PRAGMA locking_mode;", activeLocking, error)
        || !scalar(db, "PRAGMA synchronous;", activeSynchronous, error)) {
        return false;
    }
    const std::string expectedSynchronous =
        database.profile.synchronous == std::string("FULL") ? "2"
        : database.profile.synchronous == std::string("EXTRA") ? "3" : "1";
    if (activeJournal != lowerAscii(database.profile.journal)
        || activeLocking != lowerAscii(database.profile.locking)
        || activeSynchronous != expectedSynchronous) {
        error = "requested SQLite profile was not applied";
        return false;
    }
    return true;
}

bool openDatabase(const std::string &path, const Profile &profile,
                  Database &database, std::string &error)
{
    std::error_code fsError;
    fs::create_directories(fs::path(path).parent_path(), fsError);
    if (fsError) {
        error = "could not create benchmark directory";
        return false;
    }
    database.profile = profile;
    database.path = path;
    const int rc = sqlite3_open_v2(
        path.c_str(), &database.db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK || !database.db) {
        error = database.db ? sqlite3_errmsg(database.db)
                            : "sqlite open failed";
        return false;
    }
    sqlite3_extended_result_codes(database.db, 1);
    if (!configure(database, error)) {
        return false;
    }
    long long applicationId = 0;
    long long userVersion = 0;
    if (!scalarInt(database.db, "PRAGMA application_id;", applicationId,
                   error)
        || !scalarInt(database.db, "PRAGMA user_version;", userVersion,
                      error)) {
        return false;
    }
    if (applicationId == 0 && userVersion == 0) {
        return prepareSchema(database.db, error);
    }
    return applicationId == kCatalogApplicationId && userVersion == 1;
}

MediaItem fixtureItem(const std::string &id, const std::string &type,
                      const std::string &seriesId = {},
                      const std::string &seasonId = {}, int index = 0)
{
    MediaItem item;
    item.id = id;
    item.type = type;
    item.title = "benchmark";
    item.overview = "controlled fixture";
    item.etag = "fixture";
    item.seriesId = seriesId;
    item.seasonId = seasonId;
    item.indexNumber = index;
    item.parentIndexNumber = index;
    item.genres = {"benchmark"};
    item.imageTags = {{"Primary", "fixture"}};
    return item;
}

std::vector<MediaItem> fixtureSubtree(int seriesIndex, int seasonCount,
                                      int episodesPerSeason)
{
    const std::string seriesId = "benchmark-series-" + std::to_string(seriesIndex);
    std::vector<MediaItem> items;
    items.push_back(fixtureItem(seriesId, "show"));
    for (int seasonIndex = 1; seasonIndex <= seasonCount; ++seasonIndex) {
        const std::string seasonId = seriesId + "-season-"
            + std::to_string(seasonIndex);
        items.push_back(fixtureItem(seasonId, "season", seriesId, {},
                                    seasonIndex));
        for (int episodeIndex = 1; episodeIndex <= episodesPerSeason;
             ++episodeIndex) {
            items.push_back(fixtureItem(
                seasonId + "-episode-" + std::to_string(episodeIndex),
                "episode", seriesId, seasonId, episodeIndex));
        }
    }
    return items;
}

const char *kUpsertSql =
    "INSERT INTO media_items("
    "id, kind, title, overview, production_year, community_rating,"
    "etag, played, progress, playback_position_ticks, index_number,"
    "parent_index_number, runtime_ticks, series_name, series_id,"
    "season_id, art_r, art_g, art_b) "
    "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
    "?13, ?14, ?15, ?16, ?17, ?18, ?19) "
    "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, "
    "title=excluded.title, overview=excluded.overview, "
    "production_year=excluded.production_year, "
    "community_rating=excluded.community_rating, etag=excluded.etag, "
    "played=excluded.played, progress=excluded.progress, "
    "playback_position_ticks=excluded.playback_position_ticks, "
    "index_number=excluded.index_number, "
    "parent_index_number=excluded.parent_index_number, "
    "runtime_ticks=excluded.runtime_ticks, "
    "series_name=excluded.series_name, series_id=excluded.series_id, "
    "season_id=excluded.season_id, art_r=excluded.art_r, "
    "art_g=excluded.art_g, art_b=excluded.art_b";

bool writeItems(sqlite3 *db, const std::vector<MediaItem> &items,
                std::size_t &rows, std::string &error)
{
    sqlite3_stmt *upsert = nullptr;
    if (sqlite3_prepare_v2(db, kUpsertSql, -1, &upsert, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteTags = nullptr;
    sqlite3_stmt *insertTag = nullptr;
    sqlite3_stmt *selectGenres = nullptr;
    sqlite3_stmt *selectTags = nullptr;
    const bool prepared =
        sqlite3_prepare_v2(db, "DELETE FROM item_genres WHERE item_id=?1", -1,
                           &deleteGenres, nullptr) == SQLITE_OK
        && sqlite3_prepare_v2(
               db, "INSERT INTO item_genres(item_id, ordinal, genre) "
                   "VALUES(?1, ?2, ?3)", -1, &insertGenre, nullptr)
            == SQLITE_OK
        && sqlite3_prepare_v2(
               db, "DELETE FROM item_image_tags WHERE item_id=?1", -1,
               &deleteTags, nullptr) == SQLITE_OK
        && sqlite3_prepare_v2(
               db, "INSERT INTO item_image_tags(item_id, image_type, tag) "
                   "VALUES(?1, ?2, ?3)", -1, &insertTag, nullptr)
            == SQLITE_OK
        && sqlite3_prepare_v2(
               db, "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
                   "ORDER BY ordinal", -1, &selectGenres, nullptr)
            == SQLITE_OK
        && sqlite3_prepare_v2(
               db, "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
                   "ORDER BY image_type", -1, &selectTags, nullptr)
            == SQLITE_OK;
    if (!prepared) {
        error = sqlite3_errmsg(db);
        sqlite3_finalize(upsert);
        sqlite3_finalize(deleteGenres);
        sqlite3_finalize(insertGenre);
        sqlite3_finalize(deleteTags);
        sqlite3_finalize(insertTag);
        sqlite3_finalize(selectGenres);
        sqlite3_finalize(selectTags);
        return false;
    }
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteTags, insertTag, selectGenres, selectTags};
    for (const MediaItem &item : items) {
        sqlite3_reset(upsert);
        sqlite3_clear_bindings(upsert);
        MediaItemSqlError bindError = MediaItemSqlError::None;
        if (!bindMediaItemScalars(upsert, item, bindError)
            || sqlite3_step(upsert) != SQLITE_DONE) {
            error = sqlite3_errmsg(db);
            sqlite3_finalize(upsert);
            sqlite3_finalize(deleteGenres);
            sqlite3_finalize(insertGenre);
            sqlite3_finalize(deleteTags);
            sqlite3_finalize(insertTag);
            sqlite3_finalize(selectGenres);
            sqlite3_finalize(selectTags);
            return false;
        }
        ++rows;
        sqlite3_reset(upsert);
        sqlite3_clear_bindings(upsert);
        if (!replaceMediaItemCollections(collections, item, bindError)) {
            error = "benchmark collection write failed";
            sqlite3_finalize(upsert);
            sqlite3_finalize(deleteGenres);
            sqlite3_finalize(insertGenre);
            sqlite3_finalize(deleteTags);
            sqlite3_finalize(insertTag);
            sqlite3_finalize(selectGenres);
            sqlite3_finalize(selectTags);
            return false;
        }
    }
    sqlite3_finalize(upsert);
    sqlite3_finalize(deleteGenres);
    sqlite3_finalize(insertGenre);
    sqlite3_finalize(deleteTags);
    sqlite3_finalize(insertTag);
    sqlite3_finalize(selectGenres);
    sqlite3_finalize(selectTags);
    return true;
}

TransactionResult writeTransaction(sqlite3 *db,
                                   const std::vector<MediaItem> &items)
{
    TransactionResult result;
    const long long start = monotonicUs();
    if (!execSql(db, "BEGIN IMMEDIATE;", result.error)
        || !writeItems(db, items, result.rows, result.error)) {
        std::string ignored;
        execSql(db, "ROLLBACK;", ignored);
        return result;
    }
    const long long commitStart = monotonicUs();
    if (!execSql(db, "COMMIT;", result.error)) {
        std::string ignored;
        execSql(db, "ROLLBACK;", ignored);
        return result;
    }
    result.commitUs = monotonicUs() - commitStart;
    result.durationUs = monotonicUs() - start;
    result.success = true;
    return result;
}

bool rowCount(sqlite3 *db, long long &count, std::string &error)
{
    return scalarInt(db, "SELECT COUNT(*) FROM media_items;", count, error);
}

bool queryHierarchy(sqlite3 *db, std::string &error)
{
    long long ignored = 0;
    return scalarInt(
        db,
        "SELECT COUNT(*) FROM media_items WHERE series_id="
        "'benchmark-series-1';",
        ignored, error)
        && scalarInt(
               db,
               "SELECT COUNT(*) FROM media_items WHERE season_id="
               "'benchmark-series-1-season-1';",
               ignored, error);
}

bool deleteFixture(sqlite3 *db, const std::string &id, const char *kind,
                   std::string &error)
{
    const std::string sql = std::string("DELETE FROM media_items WHERE kind=")
        + kind + " AND id=?1;";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr)
        != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const bool okay = sqlite3_bind_text(statement, 1, id.c_str(), -1,
                                        SQLITE_TRANSIENT) == SQLITE_OK
        && sqlite3_step(statement) == SQLITE_DONE;
    if (!okay) {
        error = sqlite3_errmsg(db);
    }
    sqlite3_finalize(statement);
    return okay;
}

Metrics processMetrics()
{
    Metrics metrics;
    std::ifstream io("/proc/self/io");
    std::string key;
    long long value = 0;
    while (io >> key >> value) {
        if (key == "read_bytes:") metrics.readBytes = value;
        if (key == "write_bytes:") metrics.writeBytes = value;
    }
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            std::istringstream input(line.substr(7));
            input >> metrics.rssKb;
            break;
        }
    }
    struct rusage usage {
    };
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        metrics.cpuUs = static_cast<long long>(usage.ru_utime.tv_sec) * 1000000LL
            + usage.ru_utime.tv_usec
            + static_cast<long long>(usage.ru_stime.tv_sec) * 1000000LL
            + usage.ru_stime.tv_usec;
    }
    return metrics;
}

long long fileSize(const std::string &path)
{
    struct stat information {
    };
    return ::stat(path.c_str(), &information) == 0
        ? static_cast<long long>(information.st_size) : 0;
}

ArtifactSizes artifactSizes(const std::string &path)
{
    return {fileSize(path), fileSize(path + "-journal"),
            fileSize(path + "-wal"), fileSize(path + "-shm")};
}

void printArtifactSizes(const ArtifactSizes &sizes)
{
    std::printf("db_bytes=%lld journal_bytes=%lld wal_bytes=%lld shm_bytes=%lld",
                sizes.database, sizes.journal, sizes.wal, sizes.shm);
}

bool checkpointIfNeeded(Database &database, long long &durationUs,
                        int &checkpointCount, std::string &error)
{
    if (std::string(database.profile.journal) != "WAL") {
        durationUs = 0;
        checkpointCount = 0;
        return true;
    }
    int logFrames = 0;
    int checkpointedFrames = 0;
    const long long start = monotonicUs();
    const int rc = sqlite3_wal_checkpoint_v2(
        database.db, nullptr, SQLITE_CHECKPOINT_TRUNCATE, &logFrames,
        &checkpointedFrames);
    durationUs = monotonicUs() - start;
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(database.db);
        return false;
    }
    checkpointCount = 1;
    return true;
}

bool runWorkload(char profileId, const std::string &root,
                 const std::string &workload, int iteration,
                 std::string &error)
{
    const Profile *profile = profileFor(profileId);
    if (!profile) {
        error = "unknown profile";
        return false;
    }
    const std::string path = root + "/profile-" + profileId + "/"
        + workload + "-" + std::to_string(iteration) + "/catalog.sqlite3";
    Database database;
    const Metrics before = processMetrics();
    const long long openStart = monotonicUs();
    if (!openDatabase(path, *profile, database, error)) {
        return false;
    }
    const long long openUs = monotonicUs() - openStart;
    TransactionResult transaction;
    if (workload == "W1") {
        transaction = writeTransaction(database.db, fixtureSubtree(1, 2, 10));
        if (!transaction.success || !queryHierarchy(database.db, error)) {
            error = transaction.error.empty() ? error : transaction.error;
            return false;
        }
    } else if (workload == "W2") {
        transaction = writeTransaction(database.db, fixtureSubtree(1, 2, 20));
    } else if (workload == "W3") {
        transaction = writeTransaction(database.db, fixtureSubtree(1, 4, 250));
    } else if (workload.rfind("W4-", 0) == 0) {
        const int seriesCount = std::atoi(workload.substr(3).c_str());
        std::vector<MediaItem> items;
        for (int index = 1; index <= seriesCount; ++index) {
            std::vector<MediaItem> subtree = fixtureSubtree(index, 2, 20);
            items.insert(items.end(), subtree.begin(), subtree.end());
        }
        transaction = writeTransaction(database.db, items);
    } else if (workload == "W5") {
        const long long deleteStart = monotonicUs();
        if (!writeTransaction(database.db, fixtureSubtree(1, 2, 20)).success
            || !execSql(database.db, "BEGIN IMMEDIATE;", error)
            || !deleteFixture(database.db,
                              "benchmark-series-1-season-1-episode-1", "4",
                              error)
            || !deleteFixture(database.db, "benchmark-series-1-season-2", "3",
                              error)
            || !deleteFixture(database.db, "benchmark-series-1", "2", error)) {
            std::string ignored;
            execSql(database.db, "ROLLBACK;", ignored);
            return false;
        }
        const long long commitStart = monotonicUs();
        transaction.success = execSql(database.db, "COMMIT;", transaction.error);
        transaction.commitUs = monotonicUs() - commitStart;
        transaction.durationUs = monotonicUs() - deleteStart;
    } else if (workload == "W6") {
        if (!writeTransaction(database.db, fixtureSubtree(1, 2, 50)).success) {
            error = "navigation setup failed";
            return false;
        }
        const long long start = monotonicUs();
        for (int query = 0; query < 50; ++query) {
            if (!queryHierarchy(database.db, error)) return false;
        }
        transaction.success = true;
        transaction.durationUs = monotonicUs() - start;
    } else if (workload == "W7") {
        transaction.success = true;
        transaction.durationUs = openUs;
    } else {
        error = "unknown workload";
        return false;
    }
    if (!transaction.success) {
        error = transaction.error;
        return false;
    }
    long long rows = 0;
    if (!rowCount(database.db, rows, error)) return false;
    const ArtifactSizes artifacts = artifactSizes(path);
    long long checkpointUs = 0;
    int checkpointCount = 0;
    if (!checkpointIfNeeded(database, checkpointUs, checkpointCount, error)) {
        return false;
    }
    const Metrics after = processMetrics();
    std::printf("profile=%c workload=%s iteration=%d result_code=%d "
                "error_count=0 open_us=%lld duration_us=%lld "
                "commit_us=%lld checkpoint_us=%lld checkpoint_count=%d "
                "rows=%lld cpu_us=%lld rss_kb=%lld "
                "read_bytes=%lld write_bytes=%lld ",
                profileId, workload.c_str(), iteration, SQLITE_OK, openUs,
                transaction.durationUs, transaction.commitUs, checkpointUs,
                checkpointCount, rows,
                after.cpuUs - before.cpuUs, after.rssKb,
                after.readBytes - before.readBytes,
                after.writeBytes - before.writeBytes);
    printArtifactSizes(artifacts);
    std::printf(" journal=%s synchronous=%s locking=%s\n",
                profile->journal, profile->synchronous, profile->locking);
    return true;
}

bool runRecovery(char profileId, const std::string &root, int iteration,
                 std::string &error)
{
    const Profile *profile = profileFor(profileId);
    if (!profile) {
        error = "unknown profile";
        return false;
    }
    const std::string path = root + "/profile-" + profileId + "/recovery-"
        + std::to_string(iteration) + "/catalog.sqlite3";
    Database setup;
    if (!openDatabase(path, *profile, setup, error)) return false;
    // The setup connection is intentionally closed before the interruption
    // child opens the same isolated benchmark database.
    if (sqlite3_close(setup.db) != SQLITE_OK) {
        error = "benchmark setup close failed";
        return false;
    }
    setup.db = nullptr;
    pid_t child = fork();
    if (child < 0) {
        error = "fork failed";
        return false;
    }
    if (child == 0) {
        sqlite3 *db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                            nullptr) != SQLITE_OK || !db) _exit(111);
        std::string childError;
        std::size_t childRows = 0;
        if (!execSql(db, "BEGIN IMMEDIATE;", childError)
            || !writeItems(db, fixtureSubtree(1, 4, 250), childRows,
                           childError)) {
            _exit(112);
        }
        // _exit simulates abrupt process interruption while a write is active.
        _exit(113);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child
        || !WIFEXITED(status) || WEXITSTATUS(status) != 113) {
        error = "interruption fixture did not interrupt at the write point";
        return false;
    }
    Database recovered;
    if (!openDatabase(path, *profile, recovered, error)) return false;
    std::string quick;
    long long rows = 0;
    if (!scalar(recovered.db, "PRAGMA quick_check;", quick, error)
        || quick != "ok"
        || !rowCount(recovered.db, rows, error)) return false;
    sqlite3_stmt *foreignKeys = nullptr;
    const bool prepared = sqlite3_prepare_v2(
        recovered.db, "PRAGMA foreign_key_check;", -1, &foreignKeys, nullptr)
        == SQLITE_OK;
    const int fkStep = prepared ? sqlite3_step(foreignKeys) : SQLITE_ERROR;
    if (foreignKeys) sqlite3_finalize(foreignKeys);
    if (!prepared || fkStep != SQLITE_DONE) {
        error = "recovery foreign key check failed";
        return false;
    }
    const ArtifactSizes artifacts = artifactSizes(path);
    long long checkpointUs = 0;
    int checkpointCount = 0;
    if (!checkpointIfNeeded(recovered, checkpointUs, checkpointCount, error)) {
        return false;
    }
    if (sqlite3_close(recovered.db) != SQLITE_OK) {
        recovered.db = nullptr;
        error = "recovery database close failed";
        return false;
    }
    recovered.db = nullptr;
    std::printf("profile=%c workload=RECOVERY iteration=%d result_code=%d "
                "error_count=0 checkpoint_us=%lld checkpoint_count=%d "
                "rows=%lld quick_check=%s "
                "foreign_key_violations=0 ",
                profileId, iteration, SQLITE_OK, checkpointUs,
                checkpointCount, rows, quick.c_str());
    printArtifactSizes(artifacts);
    std::printf(" journal=%s synchronous=%s locking=%s\n",
                profile->journal, profile->synchronous, profile->locking);
    return true;
}

bool deterministicFixtureCheck(const std::string &root, std::string &error)
{
    const Profile *profile = profileFor('A');
    long long observed[2] = {};
    for (int run = 0; run < 2; ++run) {
        const std::string path = root + "/deterministic-" + std::to_string(run)
            + "/catalog.sqlite3";
        Database database;
        if (!openDatabase(path, *profile, database, error)) return false;
        const TransactionResult transaction =
            writeTransaction(database.db, fixtureSubtree(1, 2, 10));
        if (!transaction.success || !rowCount(database.db, observed[run], error))
            return false;
    }
    if (observed[0] != observed[1] || observed[0] != 23) {
        error = "fixture result was not deterministic";
        return false;
    }
    return true;
}

void printUsage()
{
    std::puts("usage: catalog-journal-benchmark --root <isolated-root> "
              "[--profile A-F|all] [--iterations N] [--recovery]");
}

} // namespace

bool catalogBenchmarkProfileSupported(char profile) noexcept
{
    return profileFor(profile) != nullptr;
}

bool catalogBenchmarkRootIsIsolated(const std::string &root)
{
    if (root.empty()) return false;
    const fs::path normalized = fs::absolute(fs::path(root)).lexically_normal();
    const std::string text = normalized.string();
    return text.find("miyoofin-sqlite-benchmark") != std::string::npos
        && text.find("/cache/library") == std::string::npos
        && text.find("/downloads") == std::string::npos
        && text != "/mnt/SDCARD";
}

bool runCatalogBenchmarkSelfTests(std::string &error)
{
    for (char profile = 'A'; profile <= 'F'; ++profile) {
        if (!catalogBenchmarkProfileSupported(profile)) {
            error = "supported profile rejected";
            return false;
        }
    }
    if (catalogBenchmarkProfileSupported('X')
        || !catalogBenchmarkRootIsIsolated(
               "/tmp/miyoofin-sqlite-benchmark-test")
        || catalogBenchmarkRootIsIsolated("/tmp/unrelated")) {
        error = "profile or isolation guard failed";
        return false;
    }
    const std::string root = "/tmp/miyoofin-sqlite-benchmark-self-test-"
        + std::to_string(static_cast<long long>(getpid()));
    for (char profileId = 'A'; profileId <= 'F'; ++profileId) {
        const Profile *profile = profileFor(profileId);
        const std::string acceptancePath = root + "/profile-" + profileId
            + "/acceptance/catalog.sqlite3";
        Database database;
        if (!openDatabase(acceptancePath, *profile, database, error)) {
            return false;
        }
        if ((profileId == 'E' || profileId == 'F')
            && fileSize(acceptancePath + "-shm") != 0) {
            error = "EXCLUSIVE WAL unexpectedly created an SHM sidecar";
            return false;
        }
        if (profileId == 'C') {
            const TransactionResult transaction =
                writeTransaction(database.db, fixtureSubtree(1, 1, 2));
            long long checkpointUs = 0;
            int checkpointCount = 0;
            if (!transaction.success
                || !checkpointIfNeeded(database, checkpointUs,
                                       checkpointCount, error)
                || checkpointCount != 1) {
                if (error.empty()) error = "WAL checkpoint fixture failed";
                return false;
            }
        }
    }
    if (!deterministicFixtureCheck(root, error)) return false;
    std::error_code ignored;
    fs::remove_all(root, ignored);
    return true;
}

int runCatalogJournalBenchmark(int argc, char **argv)
{
    std::string root;
    std::string profileArgument = "all";
    int iterations = 5;
    bool recovery = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help") {
            printUsage();
            return 0;
        }
        if (argument == "--root" && index + 1 < argc) {
            root = argv[++index];
        } else if (argument == "--profile" && index + 1 < argc) {
            profileArgument = argv[++index];
        } else if (argument == "--iterations" && index + 1 < argc) {
            iterations = std::atoi(argv[++index]);
        } else if (argument == "--recovery") {
            recovery = true;
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n",
                         argument.c_str());
            printUsage();
            return 2;
        }
    }
    if (!catalogBenchmarkRootIsIsolated(root) || iterations < 1) {
        std::fprintf(stderr, "benchmark root must be an isolated path and "
                            "iterations must be positive\n");
        return 2;
    }
    std::vector<char> profiles;
    if (profileArgument == "all") {
        for (char profile = 'A'; profile <= 'F'; ++profile) profiles.push_back(profile);
    } else if (profileArgument.size() == 1
               && catalogBenchmarkProfileSupported(profileArgument[0])) {
        profiles.push_back(profileArgument[0]);
    } else {
        std::fprintf(stderr, "profile must be A-F or all\n");
        return 2;
    }
    const char *workloads[] = {"W1", "W2", "W3", "W4-1", "W4-5",
                               "W4-20", "W5", "W6", "W7"};
    std::string error;
    for (char profile : profiles) {
        for (const char *workload : workloads) {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                if (!runWorkload(profile, root, workload, iteration, error)) {
                    std::fprintf(stderr, "profile %c workload %s failed: %s\n",
                                 profile, workload, error.c_str());
                    return 1;
                }
            }
        }
        if (recovery) {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                if (!runRecovery(profile, root, iteration, error)) {
                    std::fprintf(stderr, "profile %c recovery failed: %s\n",
                                 profile, error.c_str());
                    return 1;
                }
            }
        }
    }
    return 0;
}

} // namespace miyoofin
