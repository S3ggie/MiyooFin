#include "CatalogDb.hpp"

#include "../cache/LibraryCache.hpp"
#include "../data/MediaItem.hpp"
#include "../net/JellyfinApi.hpp"
#include "../download/DownloadStore.hpp"
#include "MediaItemSql.hpp"
#include "../app/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include "../../vendor/sqlite/sqlite3.h"

#include <cassert>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace miyoofin {

namespace {

const char *migrationDecisionName(CatalogDbMigrationDecision value)
{
    switch (value) {
    case CatalogDbMigrationDecision::CreateEmptyFinal: return "create_empty_final";
    case CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart: return "rebuild_migrating";
    case CatalogDbMigrationDecision::FinalDatabaseWins: return "final_database_wins";
    case CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate: return "final_wins_cleanup";
    case CatalogDbMigrationDecision::PathError: return "path_error";
    }
    return "unknown";
}

const char *scopeStatusName(CatalogDbScopeStatus value)
{
    switch (value) {
    case CatalogDbScopeStatus::Unconfigured: return "unconfigured";
    case CatalogDbScopeStatus::Pending: return "pending";
    case CatalogDbScopeStatus::Ready: return "ready";
    case CatalogDbScopeStatus::InvalidIdentity: return "invalid_identity";
    case CatalogDbScopeStatus::OpenFailed: return "open_failed";
    }
    return "unknown";
}

const char *errorCategoryName(CatalogDbErrorCategory value)
{
    switch (value) {
    case CatalogDbErrorCategory::None: return "none";
    case CatalogDbErrorCategory::InvalidIdentity: return "invalid_identity";
    case CatalogDbErrorCategory::ScopeNotReady: return "scope_not_ready";
    case CatalogDbErrorCategory::OpenFailed: return "open_failed";
    case CatalogDbErrorCategory::WrongApplicationId: return "wrong_application_id";
    case CatalogDbErrorCategory::UnsupportedVersion: return "unsupported_version";
    case CatalogDbErrorCategory::CorruptOrIo: return "corrupt_or_io";
    case CatalogDbErrorCategory::ConfigurationFailed: return "configuration_failed";
    case CatalogDbErrorCategory::SqliteError: return "sqlite_error";
    case CatalogDbErrorCategory::Superseded: return "superseded";
    }
    return "unknown";
}

const char *openStateName(CatalogDbOpenState value)
{
    switch (value) {
    case CatalogDbOpenState::NotAttempted: return "not_attempted";
    case CatalogDbOpenState::CreatedV1: return "created_v1";
    case CatalogDbOpenState::SupportedV1: return "supported_v1";
    case CatalogDbOpenState::WrongApplicationId: return "wrong_application_id";
    case CatalogDbOpenState::UnsupportedVersion: return "unsupported_version";
    case CatalogDbOpenState::CorruptOrIo: return "corrupt_or_io";
    }
    return "unknown";
}

std::string scopeDirectory(const std::string &scopeKey)
{
    return std::string("cache/library/") + scopeKey;
}

void catalogDiagnostic(const std::string &line)
{
    uiDiagnostics().log(std::string("[CatalogDb] ") + line);
}

void catalogFinalDiagnostic(bool ready, CatalogDbScopeStatus status,
                            CatalogDbErrorCategory error,
                            CatalogDbOpenState openState)
{
    char line[256];
    std::snprintf(line, sizeof(line),
                  "final_state ready=%d scope_status=%s(%u) error_category=%s(%u) open_state=%s(%u)",
                  ready ? 1 : 0, scopeStatusName(status),
                  static_cast<unsigned>(status), errorCategoryName(error),
                  static_cast<unsigned>(error), openStateName(openState),
                  static_cast<unsigned>(openState));
    catalogDiagnostic(line);
}

constexpr unsigned char kDiagnosticsOperation = 1;
constexpr unsigned char kStatementReuseOperation = 2;
constexpr unsigned char kSqlErrorOperation = 3;
constexpr unsigned char kWriteSentinelOperation = 4;
constexpr unsigned char kReadSentinelOperation = 5;
constexpr unsigned char kSchemaDiagnosticsOperation = 6;
constexpr unsigned char kWriteSchemaMarkerOperation = 7;
constexpr unsigned char kReadSchemaMarkerOperation = 8;
constexpr unsigned char kSetSchemaMetadataOperation = 9;
constexpr unsigned char kMigrationRollbackOperation = 10;
constexpr unsigned char kMediaItemCodecOperation = 11;
constexpr unsigned char kMediaItemCollectionsOperation = 12;
constexpr unsigned char kSeedHierarchyQueryOperation = 13;
constexpr unsigned char kClearHierarchyQueryOperation = 14;
constexpr std::size_t kMaxHierarchyQueryRows = 128;
constexpr std::size_t kMaxReconcileSeries = 4096;

// Checked against SQLite's current magic.txt application-ID registry on
// 2026-09-09. No MYFN entry is assigned; the value is the ASCII tag "MYFN".
constexpr std::int64_t kCatalogApplicationId = 0x4D59464E;

bool validScopeIdentity(const std::string &serverUrl, const std::string &userId)
{
    const std::string normalizedUrl = JellyfinApi::normaliseUrl(serverUrl);
    const std::size_t schemeEnd = normalizedUrl.find("://");
    return schemeEnd != std::string::npos
        && schemeEnd + 3 < normalizedUrl.size()
        && userId.find_first_not_of(" \t\r\n") != std::string::npos;
}

bool makeDirectories(const std::string &path)
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

std::string catalogPath(const std::string &scopeKey)
{
    const std::string snapshot = LibraryCache::cachePath("cache", scopeKey);
    const std::size_t slash = snapshot.find_last_of('/');
    return snapshot.substr(0, slash + 1) + "catalog.sqlite3";
}

std::string migratingPath(const std::string &scopeKey)
{
    return catalogPath(scopeKey) + ".migrating";
}

struct MigrationPathPresence {
    bool present = false;
    bool error = false;
};

MigrationPathPresence inspectMigrationPath(const std::string &path)
{
    struct stat information {
    };
    if (::stat(path.c_str(), &information) == 0) {
        return {true, false};
    }
    return {false, errno != ENOENT};
}

CatalogDbMigrationState inspectMigrationState(const std::string &scopeKey)
{
    const MigrationPathPresence final = inspectMigrationPath(
        catalogPath(scopeKey));
    const MigrationPathPresence migrating = inspectMigrationPath(
        migratingPath(scopeKey));

    CatalogDbMigrationState state;
    state.finalPresent = final.present;
    state.migratingPresent = migrating.present;
    state.pathError = final.error || migrating.error;
    if (state.pathError) {
        state.files = CatalogDbMigrationFileState::PathError;
        state.decision = CatalogDbMigrationDecision::PathError;
        return state;
    }

    const unsigned mask = (state.finalPresent ? 2u : 0u)
        | (state.migratingPresent ? 4u : 0u);
    switch (mask) {
    case 0:
        state.files = CatalogDbMigrationFileState::NoFiles;
        state.decision = CatalogDbMigrationDecision::CreateEmptyFinal;
        break;
    case 2:
        state.files = CatalogDbMigrationFileState::FinalOnly;
        state.decision = CatalogDbMigrationDecision::FinalDatabaseWins;
        break;
    case 4:
        state.files = CatalogDbMigrationFileState::MigratingOnly;
        state.decision =
            CatalogDbMigrationDecision::RebuildMigratingAtMigrationStart;
        break;
    case 6:
        state.files = CatalogDbMigrationFileState::FinalAndMigrating;
        state.decision =
            CatalogDbMigrationDecision::FinalDatabaseWinsCleanupCandidate;
        break;
    }
    state.finalWins = state.finalPresent;
    state.migratingCleanupCandidate =
        state.finalPresent && state.migratingPresent;
    return state;
}

uint64_t telemetryNowIfEnabled() noexcept
{
    return performanceTelemetry().enabledFast()
        ? TelemetryClock::monotonicUs() : 0;
}

bool mediaItemRowExists(sqlite3 *db, const std::string &id, bool &exists,
                        std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    const int prepareRc = sqlite3_prepare_v2(
        db, "SELECT 1 FROM media_items WHERE id=?1", -1, &statement, nullptr);
    if (prepareRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    const int bindRc = sqlite3_bind_text(statement, 1, id.c_str(), -1,
                                         SQLITE_TRANSIENT);
    const int stepRc = bindRc == SQLITE_OK ? sqlite3_step(statement) : bindRc;
    exists = stepRc == SQLITE_ROW;
    const int finalRc = sqlite3_finalize(statement);
    if ((stepRc != SQLITE_ROW && stepRc != SQLITE_DONE) || finalRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

struct ScalarValue {
    std::string value;
};

int scalarCallback(void *context, int columnCount, char **values, char **)
{
    if (columnCount > 0 && values[0]) {
        static_cast<ScalarValue *>(context)->value = values[0];
    }
    return 0;
}

bool scalar(sqlite3 *db, const char *sql, std::string &value,
            std::string &error)
{
    ScalarValue result;
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, scalarCallback, &result, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    value = result.value;
    return true;
}

bool exec(sqlite3 *db, const char *sql, std::string &error)
{
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
        return false;
    }
    return true;
}

bool scalarInt(sqlite3 *db, const char *sql, std::int64_t &value,
               std::string &error)
{
    std::string text;
    if (!scalar(db, sql, text, error)) {
        return false;
    }
    char *end = nullptr;
    const long long parsed = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        error = "SQLite returned a non-integer scalar";
        return false;
    }
    value = parsed;
    return true;
}

int execResult(sqlite3 *db, const char *sql, std::string &error)
{
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
    }
    return rc;
}

bool collectObjects(sqlite3 *db, std::set<std::string> &objects,
                    std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    const int rc = sqlite3_prepare_v2(
        db,
        "SELECT type, name FROM sqlite_master "
        "WHERE type IN ('table', 'index') AND name NOT LIKE 'sqlite_%' "
        "ORDER BY type, name",
        -1, &statement, nullptr);
    if (rc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const unsigned char *type = sqlite3_column_text(statement, 0);
        const unsigned char *name = sqlite3_column_text(statement, 1);
        if (type && name) {
            objects.emplace(std::string(reinterpret_cast<const char *>(type)) +
                            ":" +
                            reinterpret_cast<const char *>(name));
        }
    }
    const int finalRc = sqlite3_finalize(statement);
    if (finalRc != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    return true;
}

bool ensureSchema(sqlite3 *db, CatalogDbOpenState &openState,
                  std::string &error)
{
    std::int64_t applicationId = 0;
    std::int64_t userVersion = 0;
    if (!scalarInt(db, "PRAGMA application_id;", applicationId, error)
        || !scalarInt(db, "PRAGMA user_version;", userVersion, error)) {
        openState = CatalogDbOpenState::CorruptOrIo;
        return false;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 1) {
        openState = CatalogDbOpenState::SupportedV1;
        return true;
    }
    if (applicationId != 0 && applicationId != kCatalogApplicationId) {
        openState = CatalogDbOpenState::WrongApplicationId;
        error = "catalog database application ID is not recognized";
        return false;
    }
    if (applicationId != 0 || userVersion != 0) {
        openState = CatalogDbOpenState::UnsupportedVersion;
        error = "catalog database schema version is not supported";
        return false;
    }

    if (!exec(db, "BEGIN IMMEDIATE;", error)) {
        return false;
    }
    const char *schemaStatements[] = {
        "CREATE TABLE media_items ("
        "id TEXT PRIMARY KEY NOT NULL,"
        "kind INTEGER NOT NULL CHECK(kind BETWEEN 1 AND 4),"
        "title TEXT NOT NULL DEFAULT '',"
        "overview TEXT NOT NULL DEFAULT '',"
        "production_year INTEGER NOT NULL DEFAULT 0,"
        "community_rating REAL NOT NULL DEFAULT 0.0,"
        "etag TEXT NOT NULL DEFAULT '',"
        "played INTEGER NOT NULL DEFAULT 0 CHECK(played IN (0,1)),"
        "progress REAL NOT NULL DEFAULT 0.0,"
        "playback_position_ticks INTEGER NOT NULL DEFAULT 0,"
        "index_number INTEGER NOT NULL DEFAULT 0,"
        "parent_index_number INTEGER NOT NULL DEFAULT 0,"
        "runtime_ticks INTEGER NOT NULL DEFAULT 0,"
        "series_name TEXT NOT NULL DEFAULT '',"
        "series_id TEXT,"
        "season_id TEXT,"
        "art_r INTEGER NOT NULL DEFAULT 128 CHECK(art_r BETWEEN 0 AND 255),"
        "art_g INTEGER NOT NULL DEFAULT 128 CHECK(art_g BETWEEN 0 AND 255),"
        "art_b INTEGER NOT NULL DEFAULT 128 CHECK(art_b BETWEEN 0 AND 255),"
        "FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE,"
        "FOREIGN KEY(season_id) REFERENCES media_items(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE item_genres ("
        "item_id TEXT NOT NULL,"
        "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
        "genre TEXT NOT NULL,"
        "PRIMARY KEY(item_id, ordinal),"
        "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE item_image_tags ("
        "item_id TEXT NOT NULL,"
        "image_type TEXT NOT NULL,"
        "tag TEXT NOT NULL,"
        "PRIMARY KEY(item_id, image_type),"
        "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE hierarchy_state ("
        "series_id TEXT PRIMARY KEY NOT NULL,"
        "complete INTEGER NOT NULL CHECK(complete IN (0,1)),"
        "last_refresh_ms INTEGER NOT NULL DEFAULT 0,"
        "last_generation INTEGER NOT NULL DEFAULT 0,"
        "FOREIGN KEY(series_id) REFERENCES media_items(id) ON DELETE CASCADE"
        ");",
        "CREATE TABLE sync_state ("
        "singleton_id INTEGER PRIMARY KEY CHECK(singleton_id = 1),"
        "last_successful_ms INTEGER NOT NULL DEFAULT 0,"
        "last_reconcile_ms INTEGER NOT NULL DEFAULT 0,"
        "committed_generation INTEGER NOT NULL DEFAULT 0"
        ");",
        "CREATE INDEX idx_media_series_kind_order "
        "ON media_items(series_id, kind, index_number, id);",
        "CREATE INDEX idx_media_season_order "
        "ON media_items(season_id, index_number, id);",
        "INSERT INTO sync_state(singleton_id) VALUES(1);",
    };
    for (const char *statement : schemaStatements) {
        if (!exec(db, statement, error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
    }
    const std::string applicationIdPragma =
        "PRAGMA application_id = " + std::to_string(kCatalogApplicationId) + ";";
    if (!exec(db, applicationIdPragma.c_str(), error)
        || !exec(db, "PRAGMA user_version = 1;", error)
        || !exec(db, "COMMIT;", error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    openState = CatalogDbOpenState::CreatedV1;
    return true;
}

bool schemaConstraints(sqlite3 *db, bool &foreignKeyCascade,
                       bool &checkConstraints, std::string &error)
{
    if (!exec(db, "BEGIN;", error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind) "
                 "VALUES('__task06_series__', 2);",
                 error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind, series_id) "
                 "VALUES('__task06_season__', 3, '__task06_series__');",
                 error)
        || !exec(db,
                 "INSERT INTO media_items(id, kind, season_id) "
                 "VALUES('__task06_episode__', 4, '__task06_season__');",
                 error)
        || !exec(db,
                 "INSERT INTO item_genres(item_id, ordinal, genre) "
                 "VALUES('__task06_series__', 0, 'Test');",
                 error)
        || !exec(db,
                 "INSERT INTO item_image_tags(item_id, image_type, tag) "
                 "VALUES('__task06_series__', 'Primary', 'tag');",
                 error)
        || !exec(db,
                 "INSERT INTO hierarchy_state(series_id, complete) "
                 "VALUES('__task06_series__', 1);",
                 error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }

    std::string expectedError;
    const int foreignKeyRc = execResult(
        db,
        "INSERT INTO media_items(id, kind, season_id) "
        "VALUES('__task06_bad_fk__', 4, '__task06_missing__');",
        expectedError);
    const int foreignKeyCode = sqlite3_extended_errcode(db);
    foreignKeyCascade = (foreignKeyRc == SQLITE_CONSTRAINT
                         || foreignKeyCode == SQLITE_CONSTRAINT_FOREIGNKEY)
        && foreignKeyCode == SQLITE_CONSTRAINT_FOREIGNKEY;

    expectedError.clear();
    const int checkRc = execResult(
        db,
        "INSERT INTO media_items(id, kind) "
        "VALUES('__task06_bad_check__', 99);",
        expectedError);
    checkConstraints = checkRc == SQLITE_CONSTRAINT
        || sqlite3_extended_errcode(db) == SQLITE_CONSTRAINT_CHECK;

    if (!exec(db, "DELETE FROM media_items WHERE id='__task06_series__';",
              error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    std::int64_t remaining = 1;
    if (!scalarInt(db,
                   "SELECT (SELECT COUNT(*) FROM media_items WHERE id IN "
                   "('__task06_series__', '__task06_season__', "
                   "'__task06_episode__')) + "
                   "(SELECT COUNT(*) FROM item_genres WHERE item_id="
                   "'__task06_series__') + "
                   "(SELECT COUNT(*) FROM item_image_tags WHERE item_id="
                   "'__task06_series__') + "
                   "(SELECT COUNT(*) FROM hierarchy_state WHERE series_id="
                   "'__task06_series__');",
                   remaining, error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    foreignKeyCascade = foreignKeyCascade && remaining == 0;
    std::string ignored;
    exec(db, "ROLLBACK;", ignored);
    return true;
}

bool sameMediaItemScalars(const MediaItem &expected, const MediaItem &actual)
{
    return expected.id == actual.id && expected.type == actual.type
        && expected.title == actual.title
        && expected.overview == actual.overview && expected.year == actual.year
        && std::fabs(expected.rating - actual.rating) < 0.000001f
        && expected.etag == actual.etag && expected.played == actual.played
        && std::fabs(expected.progress - actual.progress) < 0.000001f
        && expected.playbackPositionTicks == actual.playbackPositionTicks
        && expected.indexNumber == actual.indexNumber
        && expected.parentIndexNumber == actual.parentIndexNumber
        && expected.runTimeTicks == actual.runTimeTicks
        && expected.seriesName == actual.seriesName
        && expected.seriesId == actual.seriesId
        && expected.seasonId == actual.seasonId && expected.artR == actual.artR
        && expected.artG == actual.artG && expected.artB == actual.artB;
}

bool validateHierarchyInput(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::string &error)
{
    if (series.id.empty() || series.type != "show"
        || !series.seriesId.empty() || !series.seasonId.empty()) {
        error = "invalid series hierarchy root";
        return false;
    }
    std::set<std::string> seasonIds;
    std::set<std::string> itemIds;
    itemIds.insert(series.id);
    for (const auto &season : seasons) {
        if (season.id.empty() || season.type != "season"
            || season.seriesId != series.id || !season.seasonId.empty()
            || !seasonIds.insert(season.id).second
            || !itemIds.insert(season.id).second) {
            error = "invalid season hierarchy relationship";
            return false;
        }
    }
    if (episodesBySeason.size() != seasonIds.size()) {
        error = "episode map does not match returned seasons";
        return false;
    }
    for (const auto &entry : episodesBySeason) {
        if (!seasonIds.count(entry.first)) {
            error = "episode map contains an unknown season";
            return false;
        }
        for (const auto &episode : entry.second) {
            if (episode.id.empty() || episode.type != "episode"
                || episode.seriesId != series.id
                || episode.seasonId != entry.first
                || !itemIds.insert(episode.id).second) {
                error = "invalid episode hierarchy relationship";
                return false;
            }
        }
    }
    return true;
}

bool validateReconcileInput(const std::vector<MediaItem> &series,
                            std::string &error)
{
    if (series.size() > kMaxReconcileSeries) {
        error = "authoritative series set exceeds bounded limit";
        return false;
    }
    std::set<std::string> ids;
    for (const auto &item : series) {
        if (item.id.empty() || item.type != "show"
            || !item.seriesId.empty() || !item.seasonId.empty()
            || !ids.insert(item.id).second) {
            error = "invalid authoritative series set";
            return false;
        }
    }
    return true;
}

} // namespace

struct CatalogDb::TestCommand {
    unsigned char operation;
    std::string value;
    std::promise<CatalogDbTestResult> result;
};

struct CatalogDb::QueryCommand {
    enum class Kind : unsigned char {
        Seasons,
        Episodes,
    };

    Kind kind;
    std::string parentId;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbHierarchyResult> result;
};

struct CatalogDb::HierarchyWriteCommand {
    MediaItem series;
    std::vector<MediaItem> seasons;
    std::map<std::string, std::vector<MediaItem>> episodesBySeason;
    std::uint64_t generation = 0;
    std::int64_t refreshMs = 0;
    bool complete = true;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterRows = -1;
    int cancelAfterRows = -1;
    std::promise<CatalogDbHierarchyWriteResult> result;
};

struct CatalogDb::ReconcileCommand {
    std::vector<MediaItem> series;
    bool authoritative = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterRows = -1;
    std::promise<CatalogDbReconcileResult> result;
};

struct CatalogDb::OfflineRebuildCommand {
    std::string downloadRoot;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbOfflineRebuildResult> result;
};

CatalogDb::CatalogDb()
    : m_worker(&CatalogDb::workerLoop, this)
{
    catalogDiagnostic("service_started");
}

CatalogDb::~CatalogDb()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
        for (auto &queue : m_queues) {
            queue.clear();
        }
        m_scopeCommands.clear();
        m_testCommands.clear();
        m_queryCommands.clear();
        m_writeCommands.clear();
        m_reconcileCommands.clear();
        m_offlineCommands.clear();
        m_pendingJobs = 0;
    }
    m_wake.notify_one();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

CatalogDbEnqueueResult CatalogDb::enqueueNoopForTest(
    CatalogDbPriority priority,
    const CatalogDbJobMetadata &metadata)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedStopping;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedCancelled;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            return CatalogDbEnqueueResult::RejectedFull;
        }
        m_queues[priorityIndex(priority)].push_back(
            {priority, metadata, telemetryNowIfEnabled()});
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

std::uint64_t CatalogDb::configureScope(const std::string &serverUrl,
                                        const std::string &userId)
{
    const bool validIdentity = validScopeIdentity(serverUrl, userId);
    const std::string scopeKey = validIdentity
        ? LibraryCache::scopeKey(serverUrl, userId) : std::string();
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey = scopeKey;
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = validIdentity ? CatalogDbScopeStatus::Pending
                                      : CatalogDbScopeStatus::InvalidIdentity;
        m_lastError = validIdentity ? CatalogDbErrorCategory::None
                                    : CatalogDbErrorCategory::InvalidIdentity;
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
        m_scopeCommands.clear();
        m_scopeCommands.push_back({
            validIdentity ? ScopeCommandKind::Configure
                          : ScopeCommandKind::InvalidIdentity,
            epoch, scopeKey});
    }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "configure_scope_requested epoch=%llu scope_hash=%s identity=%s",
                  static_cast<unsigned long long>(epoch),
                  scopeKey.empty() ? "none" : scopeKey.c_str(),
                  validIdentity ? "valid" : "invalid");
    catalogDiagnostic(line);
    m_wake.notify_one();
    return epoch;
}

std::uint64_t CatalogDb::deconfigureScope()
{
    std::uint64_t epoch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        epoch = ++m_requestedEpoch;
        m_requestedScopeKey.clear();
        m_activeScopeKey.clear();
        m_scopeConfigured = false;
        m_scopeReady = false;
        m_scopeStatus = CatalogDbScopeStatus::Unconfigured;
        m_lastError = CatalogDbErrorCategory::None;
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
        m_scopeCommands.clear();
        m_scopeCommands.push_back({ScopeCommandKind::Deconfigure, epoch, {}});
    }
    m_wake.notify_one();
    return epoch;
}

CatalogDbScopeState CatalogDb::scopeState() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_requestedEpoch, m_scopeConfigured, m_scopeReady, m_scopeStatus,
            m_lastError, m_openState, m_migrationState};
}

CatalogDbConnectionState CatalogDb::connectionStateForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_connectionOpen, m_connectionWorkerOwned,
            m_preparedStatementCount};
}

CatalogDbTestResult CatalogDb::runSqliteDiagnosticsForTest()
{
    return runTestCommand(kDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::runStatementReuseForTest()
{
    return runTestCommand(kStatementReuseOperation);
}

CatalogDbTestResult CatalogDb::runSqlErrorForTest()
{
    return runTestCommand(kSqlErrorOperation);
}

CatalogDbTestResult CatalogDb::runSchemaDiagnosticsForTest()
{
    return runTestCommand(kSchemaDiagnosticsOperation);
}

CatalogDbTestResult CatalogDb::writeSchemaMarkerForTest(const std::string &value)
{
    return runTestCommand(kWriteSchemaMarkerOperation, value);
}

CatalogDbTestResult CatalogDb::readSchemaMarkerForTest()
{
    return runTestCommand(kReadSchemaMarkerOperation);
}

CatalogDbTestResult CatalogDb::setSchemaMetadataForTest(
    std::int64_t applicationId, std::int64_t userVersion)
{
    return runTestCommand(kSetSchemaMetadataOperation,
                          std::to_string(applicationId) + ":"
                              + std::to_string(userVersion));
}

CatalogDbTestResult CatalogDb::runMigrationRollbackForTest()
{
    return runTestCommand(kMigrationRollbackOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCodecForTest()
{
    return runTestCommand(kMediaItemCodecOperation);
}

CatalogDbTestResult CatalogDb::runMediaItemCollectionsForTest()
{
    return runTestCommand(kMediaItemCollectionsOperation);
}

CatalogDbTestResult CatalogDb::seedHierarchyQueryFixturesForTest()
{
    return runTestCommand(kSeedHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::clearHierarchyQueryFixturesForTest()
{
    return runTestCommand(kClearHierarchyQueryOperation);
}

CatalogDbTestResult CatalogDb::writeSentinelForTest(const std::string &value)
{
    return runTestCommand(kWriteSentinelOperation, value);
}

CatalogDbTestResult CatalogDb::readSentinelForTest()
{
    return runTestCommand(kReadSentinelOperation);
}

std::future<CatalogDbHierarchyResult> CatalogDb::getSeasons(
    const std::string &seriesId, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Seasons;
    command->parentId = seriesId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbHierarchyResult> CatalogDb::getEpisodes(
    const std::string &seasonId, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<QueryCommand>();
    command->kind = QueryCommand::Kind::Episodes;
    command->parentId = seasonId;
    std::future<CatalogDbHierarchyResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb query cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb query queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        command->metadata = metadata;
        command->enqueuedMonotonicUs = telemetryNowIfEnabled();
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_queryCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbHierarchyWriteResult> CatalogDb::upsertSeriesHierarchy(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, std::int64_t refreshMs,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation,
                                 refreshMs, true, metadata, -1, -1);
}

std::future<CatalogDbHierarchyWriteResult> CatalogDb::stageSeriesHierarchy(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, std::int64_t refreshMs, bool complete,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation,
                                 refreshMs, complete, metadata, -1, -1);
}

std::future<CatalogDbHierarchyWriteResult>
CatalogDb::upsertSeriesHierarchyForTest(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, std::int64_t refreshMs, int failAfterRows,
    int cancelAfterRows)
{
    return enqueueHierarchyWrite(series, seasons, episodesBySeason, generation,
                                 refreshMs, true, {}, failAfterRows,
                                 cancelAfterRows);
}

std::future<CatalogDbHierarchyWriteResult> CatalogDb::enqueueHierarchyWrite(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::uint64_t generation, std::int64_t refreshMs,
    bool complete, const CatalogDbJobMetadata &metadata, int failAfterRows,
    int cancelAfterRows)
{
    auto command = std::make_shared<HierarchyWriteCommand>();
    command->series = series;
    command->seasons = seasons;
    command->episodesBySeason = episodesBySeason;
    command->generation = generation;
    command->refreshMs = refreshMs;
    command->complete = complete;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    command->failAfterRows = failAfterRows;
    command->cancelAfterRows = cancelAfterRows;
    std::future<CatalogDbHierarchyWriteResult> result =
        command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb hierarchy write cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbHierarchyWriteResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb write queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_writeCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeries(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata)
{
    return enqueueReconcile(series, authoritative, metadata, -1);
}

std::future<CatalogDbReconcileResult> CatalogDb::reconcileSeriesForTest(
    const std::vector<MediaItem> &series, bool authoritative,
    int failAfterRows)
{
    return enqueueReconcile(series, authoritative, {}, failAfterRows);
}

std::future<CatalogDbOfflineRebuildResult>
CatalogDb::reconstructOfflineDownloads(
    const std::string &downloadRoot, const CatalogDbJobMetadata &metadata)
{
    return enqueueOfflineRebuild(downloadRoot, metadata);
}

std::future<CatalogDbReconcileResult> CatalogDb::enqueueReconcile(
    const std::vector<MediaItem> &series, bool authoritative,
    const CatalogDbJobMetadata &metadata, int failAfterRows)
{
    auto command = std::make_shared<ReconcileCommand>();
    command->series = series;
    command->authoritative = authoritative;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    command->failAfterRows = failAfterRows;
    std::future<CatalogDbReconcileResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "CatalogDb reconciliation cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            performanceTelemetry().addCatalogDbEnqueueRejected();
            CatalogDbReconcileResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb reconciliation queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_reconcileCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

std::future<CatalogDbOfflineRebuildResult> CatalogDb::enqueueOfflineRebuild(
    const std::string &downloadRoot, const CatalogDbJobMetadata &metadata)
{
    auto command = std::make_shared<OfflineRebuildCommand>();
    command->downloadRoot = downloadRoot;
    command->metadata = metadata;
    command->enqueuedMonotonicUs = telemetryNowIfEnabled();
    std::future<CatalogDbOfflineRebuildResult> result =
        command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbOfflineRebuildResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            command->result.set_value(std::move(stopped));
            return result;
        }
        if (metadata.cancellation && metadata.cancellation->load()) {
            CatalogDbOfflineRebuildResult cancelled;
            cancelled.cancelled = true;
            cancelled.error = CatalogDbErrorCategory::Superseded;
            cancelled.message = "offline catalog reconstruction cancelled";
            command->result.set_value(std::move(cancelled));
            return result;
        }
        if (m_pendingJobs >= kMaxPendingJobs) {
            CatalogDbOfflineRebuildResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb offline reconstruction queue is full";
            command->result.set_value(std::move(full));
            return result;
        }
        if (command->metadata.generation == 0) {
            command->metadata.generation = m_generation;
        }
        if (command->metadata.scopeEpoch == 0) {
            command->metadata.scopeEpoch = m_requestedEpoch;
        }
        m_offlineCommands.push_back(command);
        ++m_pendingJobs;
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
    }
    m_wake.notify_one();
    return result;
}

CatalogDbEnqueueResult CatalogDb::enqueueScopedNoopForTest(
    CatalogDbPriority priority)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopping) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedStopping;
    }
    if (!m_scopeConfigured || !m_scopeReady) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedScopeNotReady;
    }
    if (m_pendingJobs >= kMaxPendingJobs) {
        performanceTelemetry().addCatalogDbEnqueueRejected();
        return CatalogDbEnqueueResult::RejectedFull;
    }
    CatalogDbJobMetadata metadata;
    metadata.generation = m_generation;
    metadata.scopeEpoch = m_requestedEpoch;
    m_queues[priorityIndex(priority)].push_back(
        {priority, metadata, telemetryNowIfEnabled()});
    ++m_pendingJobs;
    performanceTelemetry().setCatalogDbQueueDepth(
        static_cast<uint32_t>(m_pendingJobs));
    m_wake.notify_one();
    return CatalogDbEnqueueResult::Accepted;
}

bool CatalogDb::canPublishForTest(std::uint64_t scopeEpoch) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return scopeEpoch != 0 && scopeEpoch == m_requestedEpoch
        && m_scopeConfigured && m_scopeReady;
}

void CatalogDb::setGenerationForTest(std::uint64_t generation)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generation = generation;
    }
    m_wake.notify_one();
}

void CatalogDb::setWorkerPausedForTest(bool paused)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pausedForTest = paused;
    }
    m_wake.notify_one();
}

bool CatalogDb::waitForIdleForTest(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_idle.wait_for(lock, timeout, [this] {
        return m_pendingJobs == 0 && m_scopeCommands.empty() && !m_runningJob;
    });
}

std::vector<CatalogDbJobReport> CatalogDb::jobReportsForTest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_jobReports.begin(), m_jobReports.end()};
}

void CatalogDb::workerLoop()
{
    for (;;) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_wake.wait(lock, [this] {
            return m_stopping || (!m_pausedForTest
                && (!m_scopeCommands.empty() || !m_testCommands.empty()
                    || !m_offlineCommands.empty() || hasPendingJobsLocked()));
        });

        if (m_stopping) {
            lock.unlock();
            closeConnection();
            return;
        }

        if (!m_scopeCommands.empty()) {
            ScopeCommand command = std::move(m_scopeCommands.front());
            m_scopeCommands.pop_front();
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processScopeCommand(std::move(command));
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_testCommands.empty()) {
            std::shared_ptr<TestCommand> command = std::move(m_testCommands.front());
            m_testCommands.pop_front();
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processTestCommand(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_queryCommands.empty()) {
            std::shared_ptr<QueryCommand> command = std::move(m_queryCommands.front());
            m_queryCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processHierarchyQuery(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_writeCommands.empty()) {
            std::shared_ptr<HierarchyWriteCommand> command =
                std::move(m_writeCommands.front());
            m_writeCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processHierarchyWrite(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_reconcileCommands.empty()) {
            std::shared_ptr<ReconcileCommand> command =
                std::move(m_reconcileCommands.front());
            m_reconcileCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processReconcile(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        if (!m_offlineCommands.empty()) {
            std::shared_ptr<OfflineRebuildCommand> command =
                std::move(m_offlineCommands.front());
            m_offlineCommands.pop_front();
            --m_pendingJobs;
            performanceTelemetry().setCatalogDbQueueDepth(
                static_cast<uint32_t>(m_pendingJobs));
            m_runningJob = true;
            performanceTelemetry().setCatalogDbActive(true);
            lock.unlock();
            processOfflineRebuild(command);
            lock.lock();
            performanceTelemetry().setCatalogDbActive(false);
            m_runningJob = false;
            m_idle.notify_all();
            continue;
        }

        Job job = takeNextJobLocked();
        performanceTelemetry().setCatalogDbQueueDepth(
            static_cast<uint32_t>(m_pendingJobs));
        m_runningJob = true;
        performanceTelemetry().setCatalogDbActive(true);
        lock.unlock();

        CatalogDbJobDisposition disposition = CatalogDbJobDisposition::Completed;
        if (job.metadata.cancellation && job.metadata.cancellation->load()) {
            disposition = CatalogDbJobDisposition::Cancelled;
        } else {
            std::lock_guard<std::mutex> stateLock(m_mutex);
            if (job.metadata.generation != m_generation
                || (job.metadata.scopeEpoch != 0
                    && (job.metadata.scopeEpoch != m_requestedEpoch
                        || !m_scopeConfigured || !m_scopeReady))) {
                disposition = CatalogDbJobDisposition::Superseded;
            }
        }

        lock.lock();
        performanceTelemetry().setCatalogDbActive(false);
        const uint64_t completedUs = telemetryNowIfEnabled();
        if (job.enqueuedMonotonicUs != 0 && completedUs >= job.enqueuedMonotonicUs)
            performanceTelemetry().recordCatalogDbQueueWait(
                completedUs - job.enqueuedMonotonicUs);
        if (disposition == CatalogDbJobDisposition::Completed)
            performanceTelemetry().addCatalogDbCompleted();
        else if (disposition == CatalogDbJobDisposition::Cancelled
                 || disposition == CatalogDbJobDisposition::Superseded)
            performanceTelemetry().addCatalogDbCancelled();
        if (m_jobReports.size() == kMaxPendingJobs) {
            m_jobReports.pop_front();
        }
        m_jobReports.push_back({job.priority, job.metadata, disposition});
        m_runningJob = false;
        if (m_pendingJobs == 0) {
            m_idle.notify_all();
        }
    }
}

bool CatalogDb::hasPendingJobsLocked() const
{
    return m_pendingJobs != 0;
}

CatalogDb::Job CatalogDb::takeNextJobLocked()
{
    for (std::size_t index = 0; index < m_queues.size(); ++index) {
        if (!m_queues[index].empty()) {
            Job job = m_queues[index].front();
            m_queues[index].pop_front();
            --m_pendingJobs;
            return job;
        }
    }

    return {CatalogDbPriority::Maintenance, {}};
}

std::size_t CatalogDb::priorityIndex(CatalogDbPriority priority)
{
    return static_cast<std::size_t>(priority);
}

void CatalogDb::processScopeCommand(ScopeCommand command)
{
    {
        char line[256];
        std::snprintf(line, sizeof(line),
                      "scope_command_started epoch=%llu scope_hash=%s kind=%u",
                      static_cast<unsigned long long>(command.epoch),
                      command.scopeKey.empty() ? "none" : command.scopeKey.c_str(),
                      static_cast<unsigned>(command.kind));
        catalogDiagnostic(line);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch) {
            catalogDiagnostic("scope_command_skipped reason=stale_epoch");
            m_idle.notify_all();
            return;
        }

        m_scopeReady = false;
        m_scopeConfigured = false;
        m_scopeStatus = command.kind == ScopeCommandKind::InvalidIdentity
            ? CatalogDbScopeStatus::InvalidIdentity
            : CatalogDbScopeStatus::Pending;
        m_lastError = command.kind == ScopeCommandKind::InvalidIdentity
            ? CatalogDbErrorCategory::InvalidIdentity
            : CatalogDbErrorCategory::None;
        m_openState = CatalogDbOpenState::NotAttempted;
        m_migrationState = {};
    }

    closeConnection();

    if (command.kind == ScopeCommandKind::Deconfigure
        || command.kind == ScopeCommandKind::InvalidIdentity) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_activeScopeKey.clear();
            m_scopeConfigured = false;
            m_scopeReady = false;
            m_scopeStatus = command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbScopeStatus::InvalidIdentity
                : CatalogDbScopeStatus::Unconfigured;
            m_lastError = command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbErrorCategory::InvalidIdentity
                : CatalogDbErrorCategory::None;
        }
        catalogFinalDiagnostic(
            false,
            command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbScopeStatus::InvalidIdentity
                : CatalogDbScopeStatus::Unconfigured,
            command.kind == ScopeCommandKind::InvalidIdentity
                ? CatalogDbErrorCategory::InvalidIdentity
                : CatalogDbErrorCategory::None,
            CatalogDbOpenState::NotAttempted);
        m_idle.notify_all();
        return;
    }

    const CatalogDbMigrationState migrationState =
        inspectMigrationState(command.scopeKey);
    const bool bootstrapNeeded = !migrationState.finalPresent;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_migrationState = migrationState;
        }
    }
    {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "state epoch=%llu scope_hash=%s db_dir=%s final_present=%d migrating_present=%d bootstrap_needed=%d decision=%s(%u) path_error=%d",
                      static_cast<unsigned long long>(command.epoch),
                      command.scopeKey.c_str(), scopeDirectory(command.scopeKey).c_str(),
                      migrationState.finalPresent ? 1 : 0,
                      migrationState.migratingPresent ? 1 : 0,
                      bootstrapNeeded ? 1 : 0,
                      migrationDecisionName(migrationState.decision),
                      static_cast<unsigned>(migrationState.decision),
                      migrationState.pathError ? 1 : 0);
        catalogDiagnostic(line);
    }
    if (!bootstrapNeeded) {
        catalogDiagnostic("bootstrap_job_skipped reason=final_database_present");
    } else if (migrationState.pathError) {
        catalogDiagnostic("bootstrap_job_skipped reason=path_error");
    } else {
        catalogDiagnostic("bootstrap_job_started");
    }

    const bool opened = openConnection(command);
    const CatalogDbMigrationState completedState =
        inspectMigrationState(command.scopeKey);
    bool attempted = false;
    bool succeeded = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        attempted = m_migrationState.attempted;
        succeeded = m_migrationState.succeeded;
        if (command.epoch == m_requestedEpoch) {
            m_migrationState = completedState;
            m_migrationState.attempted = attempted;
            m_migrationState.succeeded = succeeded;
        }
    }
    if (bootstrapNeeded) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_migrationState.attempted = true;
            m_migrationState.succeeded = opened;
        }
        catalogDiagnostic(std::string("bootstrap_job_completed success=")
                          + (opened ? "1" : "0"));
    }
    m_idle.notify_all();
}

void CatalogDb::processHierarchyQuery(
    const std::shared_ptr<QueryCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbHierarchyResult result;
    result.workerOwned = true;
    const uint64_t operationStartUs = telemetryNowIfEnabled();
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (operationStartUs != 0 && endUs >= operationStartUs)
            telemetry.recordCatalogDbQuery(endUs - operationStartUs);
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (command->parentId.empty()) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        result.message = "hierarchy query requires a parent ID";
        finish();
        return;
    }

    enum class QueryState : unsigned char {
        Valid,
        Cancelled,
        Superseded,
    };
    auto state = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            return QueryState::Cancelled;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            return QueryState::Superseded;
        }
        return QueryState::Valid;
    };
    auto rejectState = [&](QueryState queryState) {
        result.error = CatalogDbErrorCategory::Superseded;
        if (queryState == QueryState::Cancelled) {
            result.cancelled = true;
            result.message = "CatalogDb query cancelled";
        } else {
            result.superseded = true;
            result.message = "CatalogDb query superseded";
        }
    };
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        rejectState(queryState);
        finish();
        return;
    }

    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };
    sqlite3_stmt *query = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    sqlite3_stmt *selectGenres = nullptr;
    sqlite3_stmt *selectImageTags = nullptr;
    const char *queryName = command->kind == QueryCommand::Kind::Seasons
        ? "hierarchy_get_seasons" : "hierarchy_get_episodes";
    const char *querySql = command->kind == QueryCommand::Kind::Seasons
        ? "SELECT id, kind, title, overview, production_year, "
          "community_rating, etag, played, progress, "
          "playback_position_ticks, index_number, parent_index_number, "
          "runtime_ticks, series_name, series_id, season_id, art_r, "
          "art_g, art_b FROM media_items WHERE series_id=?1 AND kind=3 "
          "ORDER BY index_number, title, id"
        : "SELECT id, kind, title, overview, production_year, "
          "community_rating, etag, played, progress, "
          "playback_position_ticks, index_number, parent_index_number, "
          "runtime_ticks, series_name, series_id, season_id, art_r, "
          "art_g, art_b FROM media_items WHERE season_id=?1 AND kind=4 "
          "ORDER BY index_number, title, id";
    if (!prepareCached(queryName, querySql, query)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)
        || !prepareCached(
               "media_item_genres_select",
               "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
               "ORDER BY ordinal",
               selectGenres)
        || !prepareCached(
               "media_item_image_tags_select",
               "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
               "ORDER BY image_type",
               selectImageTags)) {
        finish();
        return;
    }

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    reset(query);
    if (sqlite3_bind_text(query, 1, command->parentId.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(query);
        finish();
        return;
    }
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag,
        selectGenres, selectImageTags};
    for (;;) {
        if (const QueryState queryState = state();
            queryState != QueryState::Valid) {
            reset(query);
            rejectState(queryState);
            finish();
            return;
        }
        const int rc = sqlite3_step(query);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(query);
            finish();
            return;
        }
        if (result.items.size() >= kMaxHierarchyQueryRows) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query result exceeds bounded limit";
            reset(query);
            finish();
            return;
        }
        MediaItem item;
        MediaItemSqlError readError = MediaItemSqlError::None;
        if (!readMediaItemScalars(query, item, readError)
            || !readMediaItemCollections(collections, item, readError)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "hierarchy query row decode failed";
            reset(query);
            finish();
            return;
        }
        result.items.push_back(std::move(item));
    }
    reset(query);
    if (const QueryState queryState = state(); queryState != QueryState::Valid) {
        result.items.clear();
        rejectState(queryState);
        finish();
        return;
    }
    result.success = true;
    finish();
}

void CatalogDb::processHierarchyWrite(
    const std::shared_ptr<HierarchyWriteCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbHierarchyWriteResult result;
    result.workerOwned = true;
    uint64_t transactionStartUs = 0;
    bool transactionActive = false;
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (transactionActive && endUs >= transactionStartUs) {
            telemetry.recordCatalogDbTransaction(endUs - transactionStartUs);
            transactionActive = false;
        }
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!validateHierarchyInput(command->series, command->seasons,
                                command->episodesBySeason, result.message)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        finish();
        return;
    }

    enum class WriteState : unsigned char {
        Valid,
        Cancelled,
        Superseded,
    };
    auto state = [&] {
        if (command->cancelAfterRows >= 0
            && result.rowsWritten >= static_cast<std::size_t>(
                                         command->cancelAfterRows)) {
            return WriteState::Cancelled;
        }
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            return WriteState::Cancelled;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            return WriteState::Superseded;
        }
        return WriteState::Valid;
    };
    auto rejectState = [&](WriteState writeState) {
        result.error = CatalogDbErrorCategory::Superseded;
        if (writeState == WriteState::Cancelled) {
            result.cancelled = true;
            result.message = "CatalogDb hierarchy write cancelled";
        } else {
            result.superseded = true;
            result.message = "CatalogDb hierarchy write superseded";
        }
    };
    auto validState = [&] {
        const WriteState writeState = state();
        if (writeState != WriteState::Valid) {
            rejectState(writeState);
            return false;
        }
        return true;
    };
    if (!validState()) {
        finish();
        return;
    }

    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };
    sqlite3_stmt *upsert = nullptr;
    sqlite3_stmt *deleteSeasons = nullptr;
    sqlite3_stmt *deleteEpisodes = nullptr;
    sqlite3_stmt *markComplete = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    sqlite3_stmt *selectGenres = nullptr;
    sqlite3_stmt *selectImageTags = nullptr;
    if (!prepareCached(
            "hierarchy_upsert_item",
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
            "art_g=excluded.art_g, art_b=excluded.art_b",
            upsert)
        || !prepareCached(
               "hierarchy_delete_seasons",
               "DELETE FROM media_items WHERE kind=3 AND series_id=?1",
               deleteSeasons)
        || !prepareCached(
               "hierarchy_delete_episodes",
               "DELETE FROM media_items WHERE kind=4 AND season_id=?1",
               deleteEpisodes)
        || !prepareCached(
               "hierarchy_mark_complete",
               "INSERT INTO hierarchy_state(series_id, complete, "
               "last_refresh_ms, last_generation) VALUES(?1, ?2, ?3, ?4) "
               "ON CONFLICT(series_id) DO UPDATE SET complete=excluded.complete, "
               "last_refresh_ms=excluded.last_refresh_ms, "
               "last_generation=excluded.last_generation",
               markComplete)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)
        || !prepareCached(
               "media_item_genres_select",
               "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
               "ORDER BY ordinal",
               selectGenres)
        || !prepareCached(
               "media_item_image_tags_select",
               "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
               "ORDER BY image_type",
               selectImageTags)) {
        finish();
        return;
    }

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag,
        selectGenres, selectImageTags};
    auto rollback = [&] {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
    };
    auto deleteById = [&](sqlite3_stmt *statement, const std::string &id) {
        reset(statement);
        const bool ok = sqlite3_bind_text(statement, 1, id.c_str(), -1,
                                          SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(statement) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
        } else {
            performanceTelemetry().addCatalogDbRows(
                0, 0, static_cast<uint32_t>(sqlite3_changes(m_db)));
        }
        reset(statement);
        return ok;
    };
    auto writeItem = [&](const MediaItem &item) {
        if (!validState()) {
            return false;
        }
        MediaItemSqlError error = MediaItemSqlError::None;
        bool existed = false;
        if (performanceTelemetry().enabledFast()
            && !mediaItemRowExists(m_db, item.id, existed, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            return false;
        }
        reset(upsert);
        const bool bound = bindMediaItemScalars(upsert, item, error);
        const bool stepped = bound && sqlite3_step(upsert) == SQLITE_DONE;
        if (!stepped) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            reset(upsert);
            return false;
        }
        performanceTelemetry().addCatalogDbRows(existed ? 0u : 1u,
                                                existed ? 1u : 0u, 0u);
        reset(upsert);
        if (!replaceMediaItemCollections(collections, item, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "hierarchy collection write failed";
            return false;
        }
        ++result.rowsWritten;
        if (command->failAfterRows >= 0
            && result.rowsWritten >= static_cast<std::size_t>(
                                         command->failAfterRows)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected hierarchy write failure";
            return false;
        }
        return true;
    };

    const uint64_t beginUs = telemetryNowIfEnabled();
    const bool began = exec(m_db, "BEGIN IMMEDIATE;", result.message);
    if (began) {
        transactionStartUs = beginUs;
        transactionActive = true;
    }
    if (!began || !writeItem(command->series)
        || (command->complete
            && !deleteById(deleteSeasons, command->series.id))) {
        rollback();
        finish();
        return;
    }
    for (const auto &season : command->seasons) {
        if ((command->complete && !deleteById(deleteEpisodes, season.id))
            || !writeItem(season)) {
            rollback();
            finish();
            return;
        }
        for (const auto &episode : command->episodesBySeason.at(season.id)) {
            if (!writeItem(episode)) {
                rollback();
                finish();
                return;
            }
        }
    }
    if (!validState()) {
        rollback();
        finish();
        return;
    }
    reset(markComplete);
    if (sqlite3_bind_text(markComplete, 1, command->series.id.c_str(), -1,
                          SQLITE_TRANSIENT) != SQLITE_OK
        || sqlite3_bind_int(markComplete, 2, command->complete ? 1 : 0)
               != SQLITE_OK
        || sqlite3_bind_int64(markComplete, 3, command->refreshMs) != SQLITE_OK
        || sqlite3_bind_int64(markComplete, 4,
                              static_cast<sqlite3_int64>(command->generation))
               != SQLITE_OK
        || sqlite3_step(markComplete) != SQLITE_DONE) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(markComplete);
        rollback();
        finish();
        return;
    }
    reset(markComplete);
    const uint64_t commitStartUs = telemetryNowIfEnabled();
    if (!exec(m_db, "COMMIT;", result.message)) {
        rollback();
        finish();
        return;
    }
    transactionActive = false;
    if (commitStartUs != 0) {
        const uint64_t commitEndUs = telemetryNowIfEnabled();
        if (commitEndUs >= commitStartUs)
            performanceTelemetry().recordCatalogDbCommit(commitEndUs - commitStartUs);
    }
    if (transactionStartUs != 0) {
        const uint64_t transactionEndUs = telemetryNowIfEnabled();
        if (transactionEndUs >= transactionStartUs)
            performanceTelemetry().recordCatalogDbTransaction(
                transactionEndUs - transactionStartUs);
    }
    result.success = true;
    finish();
}

void CatalogDb::processReconcile(
    const std::shared_ptr<ReconcileCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbReconcileResult result;
    result.workerOwned = true;
    result.authoritative = command->authoritative;
    uint64_t transactionStartUs = 0;
    bool transactionActive = false;
    auto finish = [&] {
        PerformanceTelemetry &telemetry = performanceTelemetry();
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs)
            telemetry.recordCatalogDbQueueWait(endUs - command->enqueuedMonotonicUs);
        if (transactionActive && endUs >= transactionStartUs) {
            telemetry.recordCatalogDbTransaction(endUs - transactionStartUs);
            transactionActive = false;
        }
        if (result.cancelled || result.superseded)
            telemetry.addCatalogDbCancelled();
        else if (result.success)
            telemetry.addCatalogDbCompleted();
        else
            telemetry.addCatalogDbFailed();
        command->result.set_value(std::move(result));
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!command->authoritative) {
        result.skipped = true;
        result.success = true;
        result.message = "non-authoritative reconciliation skipped";
        finish();
        return;
    }
    if (!validateReconcileInput(command->series, result.message)) {
        result.error = CatalogDbErrorCategory::ConfigurationFailed;
        finish();
        return;
    }

    auto stateIsValid = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation cancelled";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "CatalogDb reconciliation superseded";
            return false;
        }
        return true;
    };
    if (!stateIsValid()) {
        finish();
        return;
    }

    if (!exec(m_db,
              "CREATE TEMP TABLE IF NOT EXISTS catalog_reconcile_ids("
              "id TEXT PRIMARY KEY NOT NULL);",
              result.message)) {
        result.error = CatalogDbErrorCategory::SqliteError;
        finish();
        return;
    }
    auto prepareCached = [&](const char *name, const char *sql,
                             sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr) != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };
    sqlite3_stmt *upsert = nullptr;
    sqlite3_stmt *clearIds = nullptr;
    sqlite3_stmt *stageId = nullptr;
    sqlite3_stmt *deleteAbsent = nullptr;
    sqlite3_stmt *deleteGenres = nullptr;
    sqlite3_stmt *insertGenre = nullptr;
    sqlite3_stmt *deleteImageTags = nullptr;
    sqlite3_stmt *insertImageTag = nullptr;
    if (!prepareCached(
            "hierarchy_upsert_item",
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
            "art_g=excluded.art_g, art_b=excluded.art_b",
            upsert)
        || !prepareCached("reconcile_clear_ids",
                          "DELETE FROM catalog_reconcile_ids", clearIds)
        || !prepareCached(
               "reconcile_stage_id",
               "INSERT INTO catalog_reconcile_ids(id) VALUES(?1)", stageId)
        || !prepareCached(
               "reconcile_delete_absent",
               "DELETE FROM media_items WHERE kind=2 AND id NOT IN "
               "(SELECT id FROM catalog_reconcile_ids)",
               deleteAbsent)
        || !prepareCached("media_item_genres_delete",
                          "DELETE FROM item_genres WHERE item_id=?1",
                          deleteGenres)
        || !prepareCached(
               "media_item_genre_insert",
               "INSERT INTO item_genres(item_id, ordinal, genre) "
               "VALUES(?1, ?2, ?3)",
               insertGenre)
        || !prepareCached("media_item_image_tags_delete",
                          "DELETE FROM item_image_tags WHERE item_id=?1",
                          deleteImageTags)
        || !prepareCached(
               "media_item_image_tag_insert",
               "INSERT INTO item_image_tags(item_id, image_type, tag) "
               "VALUES(?1, ?2, ?3)",
               insertImageTag)) {
        finish();
        return;
    }
    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    auto rollback = [&] {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
    };
    auto executeNoParameter = [&](sqlite3_stmt *statement) {
        reset(statement);
        const bool ok = sqlite3_step(statement) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        return ok;
    };
    const MediaItemCollectionStatements collections{
        deleteGenres, insertGenre, deleteImageTags, insertImageTag};
    auto writeSeries = [&](const MediaItem &item) {
        if (!stateIsValid()) {
            return false;
        }
        MediaItemSqlError error = MediaItemSqlError::None;
        bool existed = false;
        if (performanceTelemetry().enabledFast()
            && !mediaItemRowExists(m_db, item.id, existed, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            return false;
        }
        reset(upsert);
        const bool ok = bindMediaItemScalars(upsert, item, error)
            && sqlite3_step(upsert) == SQLITE_DONE;
        if (!ok) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            performanceTelemetry().recordCatalogDbSqliteError(
                sqlite3_extended_errcode(m_db));
            reset(upsert);
            return false;
        }
        performanceTelemetry().addCatalogDbRows(existed ? 0u : 1u,
                                                existed ? 1u : 0u, 0u);
        reset(upsert);
        if (!replaceMediaItemCollections(collections, item, error)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "reconciliation collection write failed";
            return false;
        }
        ++result.seriesUpserted;
        if (command->failAfterRows >= 0
            && result.seriesUpserted >= static_cast<std::size_t>(
                                            command->failAfterRows)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = "injected reconciliation failure";
            return false;
        }
        reset(stageId);
        const bool staged = sqlite3_bind_text(stageId, 1, item.id.c_str(), -1,
                                              SQLITE_TRANSIENT) == SQLITE_OK
            && sqlite3_step(stageId) == SQLITE_DONE;
        if (!staged) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(stageId);
        return staged;
    };

    const uint64_t beginUs = telemetryNowIfEnabled();
    const bool began = exec(m_db, "BEGIN IMMEDIATE;", result.message);
    if (began) {
        transactionStartUs = beginUs;
        transactionActive = true;
    }
    if (!began
        || !executeNoParameter(clearIds)) {
        rollback();
        finish();
        return;
    }
    for (const auto &item : command->series) {
        if (!writeSeries(item)) {
            rollback();
            finish();
            return;
        }
    }
    if (!stateIsValid()) {
        rollback();
        finish();
        return;
    }
    reset(deleteAbsent);
    if (sqlite3_step(deleteAbsent) != SQLITE_DONE) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        reset(deleteAbsent);
        rollback();
        finish();
        return;
    }
    result.seriesDeleted = static_cast<std::size_t>(sqlite3_changes(m_db));
    performanceTelemetry().addCatalogDbRows(
        0, 0, static_cast<uint32_t>(result.seriesDeleted));
    reset(deleteAbsent);
    const uint64_t commitStartUs = telemetryNowIfEnabled();
    if (!exec(m_db, "COMMIT;", result.message)) {
        rollback();
        finish();
        return;
    }
    transactionActive = false;
    if (commitStartUs != 0) {
        const uint64_t commitEndUs = telemetryNowIfEnabled();
        if (commitEndUs >= commitStartUs)
            performanceTelemetry().recordCatalogDbCommit(commitEndUs - commitStartUs);
    }
    if (transactionStartUs != 0) {
        const uint64_t transactionEndUs = telemetryNowIfEnabled();
        if (transactionEndUs >= transactionStartUs)
            performanceTelemetry().recordCatalogDbTransaction(
                transactionEndUs - transactionStartUs);
    }
    result.success = true;
    finish();
}

void CatalogDb::processOfflineRebuild(
    const std::shared_ptr<OfflineRebuildCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbOfflineRebuildResult result;
    result.workerOwned = true;
    uint64_t transactionStartUs = 0;
    const auto finish = [&] {
        const uint64_t endUs = telemetryNowIfEnabled();
        if (command->enqueuedMonotonicUs != 0 && endUs >= command->enqueuedMonotonicUs) {
            performanceTelemetry().recordCatalogDbQueueWait(
                endUs - command->enqueuedMonotonicUs);
        }
        if (result.cancelled || result.superseded) {
            performanceTelemetry().addCatalogDbCancelled();
        } else if (result.success) {
            performanceTelemetry().addCatalogDbCompleted();
        } else {
            performanceTelemetry().addCatalogDbFailed();
        }
        command->result.set_value(std::move(result));
    };
    const auto stateIsValid = [&] {
        if (command->metadata.cancellation
            && command->metadata.cancellation->load()) {
            result.cancelled = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "offline catalog reconstruction cancelled";
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command->metadata.generation != m_generation
            || command->metadata.scopeEpoch != m_requestedEpoch
            || !m_scopeConfigured || !m_scopeReady) {
            result.superseded = true;
            result.error = CatalogDbErrorCategory::Superseded;
            result.message = "offline catalog reconstruction superseded";
            return false;
        }
        return true;
    };
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        finish();
        return;
    }
    if (!stateIsValid()) {
        finish();
        return;
    }

    std::string scopeKey;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        scopeKey = m_requestedScopeKey;
    }
    DownloadStore store(command->downloadRoot);
    std::vector<DownloadItem> downloads;
    std::string error;
    if (!store.loadCompleteMetadata(scopeKey, downloads, &error)) {
        const std::string indexPath = store.scopePath(scopeKey) + "/index.v1";
        if (::access(indexPath.c_str(), F_OK) != 0 && errno == ENOENT) {
            result.success = true;
            result.skipped = true;
            result.message = "no durable download metadata";
            finish();
            return;
        }
        result.error = CatalogDbErrorCategory::CorruptOrIo;
        result.message = error.empty() ? "download metadata could not be read"
                                       : error;
        finish();
        return;
    }
    std::sort(downloads.begin(), downloads.end(),
              [](const DownloadItem &left, const DownloadItem &right) {
                  return left.itemId < right.itemId;
              });

    std::map<std::string, MediaItem> seriesById;
    std::map<std::string, MediaItem> seasonsById;
    std::map<std::string, std::vector<MediaItem>> episodesBySeason;
    std::vector<MediaItem> movies;
    const auto addContainer = [](std::map<std::string, MediaItem> &items,
                                 const std::string &id,
                                 const std::string &type,
                                 const std::string &title,
                                 const std::string &seriesId,
                                 std::int32_t index) {
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
    };
    for (const auto &download : downloads) {
        if (!stateIsValid()) {
            finish();
            return;
        }
        if (download.itemId.empty()) {
            continue;
        }
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
            movies.push_back(std::move(movie));
            continue;
        }
        if (download.itemType != "episode") {
            continue;
        }
        if (download.seriesId.empty() || download.seasonId.empty()) {
            continue;
        }
        addContainer(seriesById, download.seriesId, "show",
                     download.seriesName, {}, 0);
        addContainer(seasonsById, download.seasonId, "season",
                     download.seasonName, download.seriesId,
                     download.seasonNumber);
        MediaItem episode;
        episode.id = download.itemId;
        episode.type = "episode";
        episode.title = download.title.empty() ? download.itemId
                                                 : download.title;
        episode.seriesName = download.seriesName;
        episode.seriesId = download.seriesId;
        episode.seasonId = download.seasonId;
        episode.indexNumber = download.episodeNumber;
        episode.parentIndexNumber = download.seasonNumber;
        episode.runTimeTicks = download.runtimeTicks;
        episode.playbackPositionTicks = download.playbackPositionTicks;
        episode.progress = episode.runTimeTicks > 0
            ? static_cast<float>(episode.playbackPositionTicks)
                / static_cast<float>(episode.runTimeTicks)
            : 0.0f;
        episodesBySeason[download.seasonId].push_back(std::move(episode));
    }
    for (auto &entry : episodesBySeason) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const MediaItem &left, const MediaItem &right) {
                      if (left.indexNumber != right.indexNumber) {
                          return left.indexNumber < right.indexNumber;
                      }
                      return left.id < right.id;
                  });
    }

    sqlite3_stmt *upsert = nullptr;
    sqlite3_stmt *markIncomplete = nullptr;
    const auto finalize = [&] {
        if (upsert) sqlite3_finalize(upsert);
        if (markIncomplete) sqlite3_finalize(markIncomplete);
        upsert = nullptr;
        markIncomplete = nullptr;
    };
    if (sqlite3_prepare_v2(
            m_db,
            "INSERT INTO media_items(id, kind, title, overview, "
            "production_year, community_rating, etag, played, progress, "
            "playback_position_ticks, index_number, parent_index_number, "
            "runtime_ticks, series_name, series_id, season_id, art_r, art_g, "
            "art_b) VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, "
            "?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18, ?19) "
            "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, "
            "title=excluded.title, progress=excluded.progress, "
            "playback_position_ticks=excluded.playback_position_ticks, "
            "index_number=excluded.index_number, "
            "parent_index_number=excluded.parent_index_number, "
            "runtime_ticks=excluded.runtime_ticks, series_id=excluded.series_id, "
            "season_id=excluded.season_id", -1, &upsert, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(
               m_db,
               "INSERT INTO hierarchy_state(series_id, complete, "
               "last_refresh_ms, last_generation) VALUES(?1, 0, 0, ?2) "
               "ON CONFLICT(series_id) DO UPDATE SET complete=0, "
               "last_refresh_ms=0, last_generation=excluded.last_generation",
               -1, &markIncomplete, nullptr) != SQLITE_OK) {
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = sqlite3_errmsg(m_db);
        finalize();
        finish();
        return;
    }
    const auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };
    const auto writeItem = [&](const MediaItem &item) {
        MediaItemSqlError bindError = MediaItemSqlError::None;
        reset(upsert);
        if (!bindMediaItemScalars(upsert, item, bindError)
            || sqlite3_step(upsert) != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            reset(upsert);
            return false;
        }
        reset(upsert);
        ++result.itemsUpserted;
        return true;
    };
    const uint64_t beginUs = telemetryNowIfEnabled();
    if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
        result.error = CatalogDbErrorCategory::SqliteError;
        finalize();
        finish();
        return;
    }
    transactionStartUs = beginUs;
    bool writeOk = true;
    for (const auto &movie : movies) {
        if (!writeItem(movie)) {
            writeOk = false;
            break;
        }
    }
    for (const auto &entry : seriesById) {
        if (!writeOk || !writeItem(entry.second)) {
            writeOk = false;
            break;
        }
        ++result.containersSynthesized;
    }
    for (const auto &entry : seasonsById) {
        if (!writeOk || !writeItem(entry.second)) {
            writeOk = false;
            break;
        }
        ++result.containersSynthesized;
    }
    for (const auto &entry : episodesBySeason) {
        for (const auto &episode : entry.second) {
            if (!writeOk || !writeItem(episode)) {
                writeOk = false;
                break;
            }
        }
    }
    if (writeOk) {
        for (const auto &entry : seriesById) {
            reset(markIncomplete);
            if (sqlite3_bind_text(markIncomplete, 1, entry.first.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK
                || sqlite3_bind_int64(
                       markIncomplete, 2,
                       static_cast<sqlite3_int64>(command->metadata.generation))
                       != SQLITE_OK
                || sqlite3_step(markIncomplete) != SQLITE_DONE) {
                result.error = CatalogDbErrorCategory::SqliteError;
                result.message = sqlite3_errmsg(m_db);
                writeOk = false;
                break;
            }
        }
    }
    if (writeOk && !stateIsValid()) {
        writeOk = false;
    }
    if (!writeOk) {
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
        finalize();
        if (result.error == CatalogDbErrorCategory::None) {
            result.error = result.cancelled || result.superseded
                ? CatalogDbErrorCategory::Superseded
                : CatalogDbErrorCategory::SqliteError;
        }
        finish();
        return;
    }
    if (!exec(m_db, "COMMIT;", result.message)) {
        result.error = CatalogDbErrorCategory::SqliteError;
        std::string ignored;
        exec(m_db, "ROLLBACK;", ignored);
        finalize();
        finish();
        return;
    }
    finalize();
    result.success = true;
    if (transactionStartUs != 0) {
        const uint64_t endUs = telemetryNowIfEnabled();
        if (endUs >= transactionStartUs) {
            performanceTelemetry().recordCatalogDbTransaction(
                endUs - transactionStartUs);
        }
    }
    finish();
}

void CatalogDb::closeConnection()
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    if (!m_db) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_connectionOpen = false;
        m_connectionWorkerOwned = false;
        m_preparedStatementCount = 0;
        return;
    }

    finalizeStatements();
    sqlite3_close(m_db);
    m_db = nullptr;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_connectionOpen = false;
    m_connectionWorkerOwned = false;
    m_preparedStatementCount = 0;
}

void CatalogDb::finalizeStatements()
{
    // Future schema migrations must use this worker-only boundary, then run
    // BEGIN IMMEDIATE, migrate, set user_version, COMMIT, and rebuild the
    // registry before publishing the new schema-ready state.
    assert(std::this_thread::get_id() == m_worker.get_id());
    for (auto &entry : m_statements) {
        sqlite3_finalize(entry.second);
    }
    m_statements.clear();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_preparedStatementCount = 0;
}

bool CatalogDb::openConnection(const ScopeCommand &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            return false;
        }
    }

    const std::string path = catalogPath(command.scopeKey);
    CatalogDbMigrationState migrationState =
        inspectMigrationState(command.scopeKey);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            const bool attempted = m_migrationState.attempted;
            const bool succeeded = m_migrationState.succeeded;
            m_migrationState = migrationState;
            m_migrationState.attempted = attempted;
            m_migrationState.succeeded = succeeded;
        }
    }
    if (migrationState.pathError) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_skipped reason=path_error");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }
    const std::size_t slash = path.find_last_of('/');
    std::string error;
    if (slash == std::string::npos || !makeDirectories(path.substr(0, slash))) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_skipped reason=directory_create_failed");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }

    bool bootstrapped = false;
    if (!migrationState.finalPresent) {
        std::string bootstrapError;
        if (!bootstrapFreshDatabaseForWorker(command, bootstrapError)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (command.epoch == m_requestedEpoch) {
                m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                m_openState = CatalogDbOpenState::CorruptOrIo;
            }
            catalogDiagnostic(std::string("bootstrap_failed reason=")
                              + (bootstrapError.empty() ? "unknown"
                                                         : bootstrapError));
            catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                   CatalogDbErrorCategory::CorruptOrIo,
                                   CatalogDbOpenState::CorruptOrIo);
            return false;
        }
        bootstrapped = true;
        migrationState = inspectMigrationState(command.scopeKey);
        if (migrationState.pathError || !migrationState.finalPresent) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (command.epoch == m_requestedEpoch) {
                m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                m_openState = CatalogDbOpenState::CorruptOrIo;
            }
            catalogDiagnostic("bootstrap_failed reason=promotion_not_visible");
            catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                   CatalogDbErrorCategory::CorruptOrIo,
                                   CatalogDbOpenState::CorruptOrIo);
            return false;
        }
    }

    catalogDiagnostic(std::string("sqlite_open_attempt db_dir=")
                      + scopeDirectory(command.scopeKey));
    sqlite3 *db = nullptr;
    const int openRc = sqlite3_open_v2(
        path.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    {
        char line[128];
        std::snprintf(line, sizeof(line),
                      "sqlite_open_returned rc=%d handle=%d", openRc,
                      db != nullptr ? 1 : 0);
        catalogDiagnostic(line);
    }
    if (openRc != SQLITE_OK || !db) {
        if (db) {
            sqlite3_close(db);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }

    sqlite3_extended_result_codes(db, 1);
    if (!exec(db, "PRAGMA foreign_keys = ON;", error)
        || !exec(db, "PRAGMA trusted_schema = OFF;", error)
        || !exec(db, "PRAGMA journal_mode = DELETE;", error)
        || !exec(db, "PRAGMA synchronous = FULL;", error)
        || !exec(db, "PRAGMA locking_mode = NORMAL;", error)) {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::CorruptOrIo;
            m_openState = CatalogDbOpenState::CorruptOrIo;
        }
        catalogDiagnostic("sqlite_open_failed stage=configuration");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::CorruptOrIo,
                               CatalogDbOpenState::CorruptOrIo);
        return false;
    }

    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    if (!scalar(db, "PRAGMA foreign_keys;", foreignKeys, error)
        || !scalar(db, "PRAGMA trusted_schema;", trustedSchema, error)
        || !scalar(db, "PRAGMA journal_mode;", journalMode, error)
        || !scalar(db, "PRAGMA synchronous;", synchronous, error)
        || !scalar(db, "PRAGMA locking_mode;", lockingMode, error)
        || foreignKeys != "1" || trustedSchema != "0"
        || journalMode != "delete" || synchronous != "2"
        || lockingMode != "normal") {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_lastError = CatalogDbErrorCategory::ConfigurationFailed;
        }
        catalogDiagnostic("sqlite_open_failed stage=configuration_validation");
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               CatalogDbErrorCategory::ConfigurationFailed,
                               CatalogDbOpenState::NotAttempted);
        return false;
    }

    CatalogDbOpenState openState = CatalogDbOpenState::NotAttempted;
    if (!ensureSchema(db, openState, error)) {
        sqlite3_close(db);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch == m_requestedEpoch) {
            m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
            m_openState = openState;
            m_lastError = openState == CatalogDbOpenState::WrongApplicationId
                ? CatalogDbErrorCategory::WrongApplicationId
                : openState == CatalogDbOpenState::UnsupportedVersion
                    ? CatalogDbErrorCategory::UnsupportedVersion
                    : CatalogDbErrorCategory::CorruptOrIo;
        }
        const CatalogDbErrorCategory errorCategory =
            openState == CatalogDbOpenState::WrongApplicationId
                ? CatalogDbErrorCategory::WrongApplicationId
                : openState == CatalogDbOpenState::UnsupportedVersion
                    ? CatalogDbErrorCategory::UnsupportedVersion
                    : CatalogDbErrorCategory::CorruptOrIo;
        char line[256];
        std::snprintf(line, sizeof(line),
                      "sqlite_open_failed stage=schema error_category=%s(%u) open_state=%s(%u)",
                      errorCategoryName(errorCategory),
                      static_cast<unsigned>(errorCategory),
                      openStateName(openState), static_cast<unsigned>(openState));
        catalogDiagnostic(line);
        catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                               errorCategory, openState);
        return false;
    }
    if (bootstrapped) {
        openState = CatalogDbOpenState::CreatedV1;
    }

    if (migrationState.migratingPresent) {
        const std::string temporaryPath = migratingPath(command.scopeKey);
        const std::string sidecars[] = {
            temporaryPath, temporaryPath + "-journal",
            temporaryPath + "-wal", temporaryPath + "-shm"};
        for (const auto &candidate : sidecars) {
            if (std::remove(candidate.c_str()) != 0 && errno != ENOENT) {
                sqlite3_close(db);
                std::lock_guard<std::mutex> lock(m_mutex);
                if (command.epoch == m_requestedEpoch) {
                    m_scopeStatus = CatalogDbScopeStatus::OpenFailed;
                    m_lastError = CatalogDbErrorCategory::CorruptOrIo;
                    m_openState = CatalogDbOpenState::CorruptOrIo;
                }
                catalogDiagnostic("sqlite_open_failed stage=stale_temp_cleanup");
                catalogFinalDiagnostic(false, CatalogDbScopeStatus::OpenFailed,
                                       CatalogDbErrorCategory::CorruptOrIo,
                                       CatalogDbOpenState::CorruptOrIo);
                return false;
            }
        }
        catalogDiagnostic("stale_temp_cleanup completed");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (command.epoch != m_requestedEpoch || m_stopping) {
            sqlite3_close(db);
            return false;
        }
        m_db = db;
        m_connectionOpen = true;
        m_connectionWorkerOwned = std::this_thread::get_id() == m_worker.get_id();
        m_activeScopeKey = command.scopeKey;
        m_scopeConfigured = true;
        m_scopeReady = true;
        m_scopeStatus = CatalogDbScopeStatus::Ready;
        m_lastError = CatalogDbErrorCategory::None;
        m_openState = openState;
    }
    catalogFinalDiagnostic(true, CatalogDbScopeStatus::Ready,
                           CatalogDbErrorCategory::None, openState);
    return true;
}

bool CatalogDb::bootstrapFreshDatabaseForWorker(const ScopeCommand &command,
                                                std::string &error)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    const auto scopeIsCurrent = [&] {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_stopping && command.epoch == m_requestedEpoch
            && command.scopeKey == m_requestedScopeKey;
    };
    const std::string finalPath = catalogPath(command.scopeKey);
    const std::string temporaryPath = migratingPath(command.scopeKey);
    const std::string sidecars[] = {
        temporaryPath, temporaryPath + "-journal",
        temporaryPath + "-wal", temporaryPath + "-shm"};
    const auto removeTemporaryFamily = [&] {
        for (const auto &candidate : sidecars) {
            if (std::remove(candidate.c_str()) != 0 && errno != ENOENT) {
                error = "could not remove stale temporary catalog";
                return false;
            }
        }
        return true;
    };
    const CatalogDbMigrationState initialState =
        inspectMigrationState(command.scopeKey);
    if (initialState.pathError) {
        error = "catalog database paths are not accessible";
        return false;
    }
    if (initialState.finalPresent) {
        return true;
    }
    if (!scopeIsCurrent()) {
        error = "fresh catalog bootstrap scope was superseded";
        return false;
    }
    if (!removeTemporaryFamily()) {
        return false;
    }

    const std::size_t slash = temporaryPath.find_last_of('/');
    if (slash == std::string::npos
        || !makeDirectories(temporaryPath.substr(0, slash))) {
        error = "could not create catalog database directory";
        return false;
    }
    sqlite3 *temporary = nullptr;
    const int openRc = sqlite3_open_v2(
        temporaryPath.c_str(), &temporary,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (openRc != SQLITE_OK || !temporary) {
        if (temporary) {
            sqlite3_close(temporary);
        }
        error = "could not open temporary catalog database";
        return false;
    }
    sqlite3_extended_result_codes(temporary, 1);
    const auto closeTemporary = [&] {
        if (temporary) {
            sqlite3_close(temporary);
            temporary = nullptr;
        }
    };
    if (!exec(temporary, "PRAGMA foreign_keys = ON;", error)
        || !exec(temporary, "PRAGMA trusted_schema = OFF;", error)
        || !exec(temporary, "PRAGMA journal_mode = DELETE;", error)
        || !exec(temporary, "PRAGMA synchronous = FULL;", error)
        || !exec(temporary, "PRAGMA locking_mode = NORMAL;", error)) {
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    CatalogDbOpenState openState = CatalogDbOpenState::NotAttempted;
    if (!ensureSchema(temporary, openState, error)
        || openState != CatalogDbOpenState::CreatedV1) {
        if (error.empty()) {
            error = "fresh catalog schema creation failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    std::string foreignKeys;
    std::string trustedSchema;
    std::string journalMode;
    std::string synchronous;
    std::string lockingMode;
    if (!scalar(temporary, "PRAGMA foreign_keys;", foreignKeys, error)
        || !scalar(temporary, "PRAGMA trusted_schema;", trustedSchema, error)
        || !scalar(temporary, "PRAGMA journal_mode;", journalMode, error)
        || !scalar(temporary, "PRAGMA synchronous;", synchronous, error)
        || !scalar(temporary, "PRAGMA locking_mode;", lockingMode, error)
        || foreignKeys != "1" || trustedSchema != "0"
        || journalMode != "delete" || synchronous != "2"
        || lockingMode != "normal") {
        if (error.empty()) {
            error = "fresh catalog configuration validation failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    std::string quickCheck;
    if (!scalar(temporary, "PRAGMA quick_check;", quickCheck, error)
        || quickCheck != "ok") {
        if (error.empty()) {
            error = "fresh catalog quick_check failed";
        }
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    sqlite3_stmt *foreignKeyCheck = nullptr;
    const bool foreignKeyPrepared = sqlite3_prepare_v2(
        temporary, "PRAGMA foreign_key_check;", -1, &foreignKeyCheck, nullptr)
        == SQLITE_OK;
    const int foreignKeyRc = foreignKeyPrepared
        ? sqlite3_step(foreignKeyCheck) : SQLITE_ERROR;
    if (foreignKeyCheck) {
        sqlite3_finalize(foreignKeyCheck);
    }
    if (!foreignKeyPrepared || foreignKeyRc != SQLITE_DONE) {
        error = "fresh catalog foreign_key_check failed";
        closeTemporary();
        removeTemporaryFamily();
        return false;
    }
    if (sqlite3_close(temporary) != SQLITE_OK) {
        temporary = nullptr;
        error = "temporary catalog database did not close cleanly";
        removeTemporaryFamily();
        return false;
    }
    temporary = nullptr;
    const int temporaryFd = ::open(temporaryPath.c_str(), O_RDONLY);
    if (temporaryFd < 0 || ::fsync(temporaryFd) != 0) {
        if (temporaryFd >= 0) {
            ::close(temporaryFd);
        }
        error = "temporary catalog fsync failed";
        removeTemporaryFamily();
        return false;
    }
    ::close(temporaryFd);
    if (!scopeIsCurrent()) {
        error = "fresh catalog bootstrap scope was superseded";
        removeTemporaryFamily();
        return false;
    }
    const CatalogDbMigrationState beforePromotion =
        inspectMigrationState(command.scopeKey);
    if (beforePromotion.pathError) {
        error = "catalog database paths became inaccessible";
        removeTemporaryFamily();
        return false;
    }
    if (beforePromotion.finalPresent) {
        return removeTemporaryFamily();
    }
    if (::rename(temporaryPath.c_str(), finalPath.c_str()) != 0) {
        error = "fresh catalog promotion failed";
        removeTemporaryFamily();
        return false;
    }
    const int directoryFd = ::open(finalPath.substr(0, finalPath.find_last_of('/')).c_str(),
                                   O_RDONLY);
    if (directoryFd >= 0) {
        ::fsync(directoryFd);
        ::close(directoryFd);
    }
    return true;
}

void CatalogDb::processTestCommand(const std::shared_ptr<TestCommand> &command)
{
    assert(std::this_thread::get_id() == m_worker.get_id());
    CatalogDbTestResult result;
    result.workerOwned = std::this_thread::get_id() == m_worker.get_id();
    if (!m_db) {
        result.error = CatalogDbErrorCategory::ScopeNotReady;
        result.message = "CatalogDb has no ready scoped connection";
        command->result.set_value(std::move(result));
        return;
    }

    auto prepareNamed = [&](const char *name, const char *sql,
                            sqlite3_stmt *&statement) {
        auto existing = m_statements.find(name);
        if (existing != m_statements.end()) {
            statement = existing->second;
            return true;
        }
        const int rc = sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            return false;
        }
        m_statements.emplace(name, statement);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        return true;
    };

    auto reset = [](sqlite3_stmt *statement) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
    };

    switch (command->operation) {
    case kDiagnosticsOperation:
        if (!scalar(m_db, "PRAGMA foreign_keys;", result.foreignKeys,
                    result.message)
            || !scalar(m_db, "PRAGMA trusted_schema;", result.trustedSchema,
                       result.message)
            || !scalar(m_db, "PRAGMA journal_mode;", result.journalMode,
                       result.message)
            || !scalar(m_db, "PRAGMA synchronous;", result.synchronous,
                       result.message)
            || !scalar(m_db, "PRAGMA locking_mode;", result.lockingMode,
                       result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        result.success = result.foreignKeys == "1"
            && result.trustedSchema == "0"
            && result.journalMode == "delete"
            && result.synchronous == "2"
            && result.lockingMode == "normal";
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "connection pragmas do not match the baseline";
        }
        break;

    case kSchemaDiagnosticsOperation: {
        std::set<std::string> objects;
        if (!collectObjects(m_db, objects, result.message)
            || !scalarInt(m_db, "PRAGMA application_id;",
                          result.applicationId, result.message)
            || !scalarInt(m_db, "PRAGMA user_version;", result.userVersion,
                          result.message)
            || !scalar(m_db,
                       "SELECT singleton_id FROM sync_state "
                       "WHERE singleton_id=1;",
                       result.sentinel, result.message)
            || !schemaConstraints(m_db, result.foreignKeyCascade,
                                  result.checkConstraints, result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        const std::set<std::string> expectedObjects = {
            "index:idx_media_season_order",
            "index:idx_media_series_kind_order",
            "table:hierarchy_state",
            "table:item_genres",
            "table:item_image_tags",
            "table:media_items",
            "table:sync_state",
        };
        result.exactSchema = objects == expectedObjects;
        result.singletonSeeded = result.sentinel == "1";
        result.success = result.exactSchema
            && result.foreignKeyCascade && result.checkConstraints
            && result.singletonSeeded
            && result.applicationId == kCatalogApplicationId
            && result.userVersion == 1;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "schema v1 diagnostics did not match the contract";
        }
        break;
    }

    case kStatementReuseOperation: {
        sqlite3_stmt *first = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", first)) {
            break;
        }
        int rc = sqlite3_step(first);
        reset(first);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
            break;
        }
        sqlite3_stmt *second = nullptr;
        if (!prepareNamed("test_scalar", "SELECT 1", second)) {
            break;
        }
        reset(second);
        result.statementReused = first == second;
        result.success = result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "statement registry did not reuse the statement";
        }
        break;
    }

    case kSqlErrorOperation: {
        sqlite3_stmt *statement = nullptr;
        const int rc = sqlite3_prepare_v2(
            m_db, "SELECT * FROM catalog_task05_missing_table", -1,
            &statement, nullptr);
        int stepRc = rc;
        if (rc == SQLITE_OK) {
            stepRc = sqlite3_step(statement);
        }
        if (statement) {
            sqlite3_finalize(statement);
        }
        const bool sqliteFailed = stepRc != SQLITE_OK && stepRc != SQLITE_ROW
            && stepRc != SQLITE_DONE;
        result.success = false;
        result.error = sqliteFailed ? CatalogDbErrorCategory::SqliteError
                                    : CatalogDbErrorCategory::ConfigurationFailed;
        result.message = sqliteFailed ? sqlite3_errmsg(m_db)
                                      : "expected SQLite error was not returned";
        break;
    }

    case kMediaItemCodecOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT OR REPLACE INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19)",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)) {
            break;
        }

        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto roundTrip = [&](const MediaItem &expected,
                             bool nullableRelationships) {
            MediaItemSqlError bindError = MediaItemSqlError::None;
            if (!bindMediaItemScalars(insert, expected, bindError)) {
                result.message = "MediaItem scalar bind failed";
                reset(insert);
                return false;
            }
            if (nullableRelationships
                && (sqlite3_bind_null(insert, 15) != SQLITE_OK
                    || sqlite3_bind_null(insert, 16) != SQLITE_OK)) {
                result.message = "MediaItem nullable bind failed";
                reset(insert);
                return false;
            }
            const int insertRc = sqlite3_step(insert);
            reset(insert);
            if (insertRc != SQLITE_DONE) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                return false;
            }

            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK) {
                result.message = "MediaItem scalar lookup bind failed";
                reset(select);
                return false;
            }
            const int selectRc = sqlite3_step(select);
            if (selectRc != SQLITE_ROW) {
                result.message = expected.id + ": " + sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk) {
                result.message = expected.id + ": scalar read failed";
                return false;
            }
            if (!sameMediaItemScalars(expected, actual)) {
                result.message = expected.id + ": scalar comparison failed";
                return false;
            }
            return true;
        };

        MediaItem series;
        series.id = "__task08_series__";
        series.type = "show";
        series.title = "Full Series";
        series.overview = "Series overview";
        series.year = 2024;
        series.rating = 8.25f;
        series.etag = "series-etag";
        series.played = true;
        series.progress = 0.75f;
        series.playbackPositionTicks = 123456789;
        series.indexNumber = 7;
        series.parentIndexNumber = 8;
        series.runTimeTicks = 987654321;
        series.seriesName = "Series name";
        series.artR = 10;
        series.artG = 20;
        series.artB = 30;

        MediaItem season = series;
        season.id = "__task08_season__";
        season.type = "season";
        season.seriesId = series.id;
        season.title = "Full Season";

        MediaItem episode = season;
        episode.id = "__task08_episode__";
        episode.type = "episode";
        episode.seasonId = season.id;
        episode.title = "Full Episode";

        MediaItem movie = series;
        movie.id = "__task08_movie__";
        movie.type = "movie";
        movie.seriesName.clear();
        movie.seriesId.clear();
        movie.seasonId.clear();
        movie.title = "Full Movie";

        result.codecPopulatedKinds = roundTrip(series, false)
            && roundTrip(season, false) && roundTrip(episode, false)
            && roundTrip(movie, false);

        MediaItem defaults;
        defaults.id = "__task08_defaults__";
        defaults.type = "movie";
        result.codecDefaults = roundTrip(defaults, true);
        result.codecNullableRelationships = result.codecDefaults
            && defaults.seriesId.empty() && defaults.seasonId.empty();

        MediaItem boundaries;
        boundaries.id = "__task08_boundaries__";
        boundaries.type = "episode";
        boundaries.year = std::numeric_limits<int>::min();
        boundaries.rating = 10.0f;
        boundaries.progress = 1.0f;
        boundaries.playbackPositionTicks = std::numeric_limits<long long>::min();
        boundaries.indexNumber = std::numeric_limits<int>::min();
        boundaries.parentIndexNumber = std::numeric_limits<int>::max();
        boundaries.runTimeTicks = std::numeric_limits<long long>::max();
        boundaries.artR = 0;
        boundaries.artG = 1;
        boundaries.artB = 255;
        result.codecBoundaries = roundTrip(boundaries, false);

        MediaItem invalidKind;
        invalidKind.id = "__task08_invalid_kind__";
        invalidKind.type = "unknown";
        MediaItemSqlError invalidError = MediaItemSqlError::None;
        MediaItemSqlError invalidReadError = MediaItemSqlError::None;
        result.codecInvalidKind = !bindMediaItemScalars(insert, invalidKind,
                                                         invalidError)
            && invalidError == MediaItemSqlError::InvalidKind
            && mediaItemKindFromSql(99, invalidReadError).empty()
            && invalidReadError == MediaItemSqlError::InvalidKind;
        reset(insert);

        MediaItem missingId;
        missingId.type = "movie";
        MediaItemSqlError missingError = MediaItemSqlError::None;
        result.codecMissingId = !bindMediaItemScalars(insert, missingId,
                                                       missingError)
            && missingError == MediaItemSqlError::MissingId;
        reset(insert);

        exec(m_db, "DELETE FROM media_items WHERE id LIKE '__task08_%';",
             result.message);
        result.success = result.codecPopulatedKinds && result.codecDefaults
            && result.codecBoundaries && result.codecInvalidKind
            && result.codecMissingId && result.codecNullableRelationships;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem scalar codec checks failed";
            }
        }
        break;
    }

    case kMediaItemCollectionsOperation: {
        sqlite3_stmt *insert = nullptr;
        sqlite3_stmt *select = nullptr;
        sqlite3_stmt *deleteGenres = nullptr;
        sqlite3_stmt *insertGenre = nullptr;
        sqlite3_stmt *deleteImageTags = nullptr;
        sqlite3_stmt *insertImageTag = nullptr;
        sqlite3_stmt *selectGenres = nullptr;
        sqlite3_stmt *selectImageTags = nullptr;
        sqlite3_stmt *deleteItem = nullptr;
        if (!prepareNamed(
                "media_item_scalar_insert",
                "INSERT OR REPLACE INTO media_items("
                "id, kind, title, overview, production_year, community_rating,"
                "etag, played, progress, playback_position_ticks, index_number,"
                "parent_index_number, runtime_ticks, series_name, series_id,"
                "season_id, art_r, art_g, art_b) "
                "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, "
                "?13, ?14, ?15, ?16, ?17, ?18, ?19)",
                insert)
            || !prepareNamed(
                "media_item_scalar_select",
                "SELECT id, kind, title, overview, production_year, "
                "community_rating, etag, played, progress, "
                "playback_position_ticks, index_number, parent_index_number, "
                "runtime_ticks, series_name, series_id, season_id, art_r, "
                "art_g, art_b FROM media_items WHERE id=?1",
                select)
            || !prepareNamed(
                "media_item_genres_delete",
                "DELETE FROM item_genres WHERE item_id=?1", deleteGenres)
            || !prepareNamed(
                "media_item_genre_insert",
                "INSERT INTO item_genres(item_id, ordinal, genre) "
                "VALUES(?1, ?2, ?3)", insertGenre)
            || !prepareNamed(
                "media_item_image_tags_delete",
                "DELETE FROM item_image_tags WHERE item_id=?1",
                deleteImageTags)
            || !prepareNamed(
                "media_item_image_tag_insert",
                "INSERT INTO item_image_tags(item_id, image_type, tag) "
                "VALUES(?1, ?2, ?3)", insertImageTag)
            || !prepareNamed(
                "media_item_genres_select",
                "SELECT ordinal, genre FROM item_genres WHERE item_id=?1 "
                "ORDER BY ordinal", selectGenres)
            || !prepareNamed(
                "media_item_image_tags_select",
                "SELECT image_type, tag FROM item_image_tags WHERE item_id=?1 "
                "ORDER BY image_type", selectImageTags)
            || !prepareNamed(
                "media_item_delete",
                "DELETE FROM media_items WHERE id=?1", deleteItem)) {
            break;
        }

        const MediaItemCollectionStatements collections{
            deleteGenres, insertGenre, deleteImageTags, insertImageTag,
            selectGenres, selectImageTags};
        auto reset = [](sqlite3_stmt *statement) {
            sqlite3_reset(statement);
            sqlite3_clear_bindings(statement);
        };
        auto rollback = [&] {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
        };
        auto insertAndRead = [&](const MediaItem &expected) {
            if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
                return false;
            }
            MediaItemSqlError error = MediaItemSqlError::None;
            reset(insert);
            if (!bindMediaItemScalars(insert, expected, error)
                || sqlite3_step(insert) != SQLITE_DONE) {
                result.message = sqlite3_errmsg(m_db);
                reset(insert);
                rollback();
                return false;
            }
            reset(insert);
            if (!replaceMediaItemCollections(collections, expected, error)) {
                result.message = "MediaItem collection replacement failed";
                rollback();
                return false;
            }
            if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                return false;
            }

            reset(select);
            if (sqlite3_bind_text(select, 1, expected.id.c_str(), -1,
                                  SQLITE_TRANSIENT) != SQLITE_OK
                || sqlite3_step(select) != SQLITE_ROW) {
                result.message = sqlite3_errmsg(m_db);
                reset(select);
                return false;
            }
            MediaItem actual;
            MediaItemSqlError readError = MediaItemSqlError::None;
            const bool readOk = readMediaItemScalars(select, actual, readError);
            reset(select);
            if (!readOk || !readMediaItemCollections(collections, actual,
                                                     readError)) {
                result.message = "MediaItem collection read failed";
                return false;
            }
            return mediaItemsEquivalentForCatalog(expected, actual);
        };

        MediaItem item;
        item.id = "__task09_item__";
        item.type = "movie";
        item.title = "Collection Fixture";

        result.collectionsZero = insertAndRead(item);

        item.genres = {"Drama", "Science Fiction", "Thriller"};
        item.genre = item.genres.front();
        item.imageTags = {{"Backdrop", "backdrop-tag"},
                          {"Primary", "primary-tag"},
                          {"Thumb", "thumb-tag"}};
        result.collectionsMultiple = insertAndRead(item);

        MediaItem updated = item;
        updated.genres = {"Updated"};
        updated.genre = updated.genres.front();
        updated.imageTags.clear();
        result.collectionsUpdateRemoval = insertAndRead(updated);
        if (result.collectionsUpdateRemoval) {
            result.collectionsUpdateRemoval = updated.imageTags.empty()
                && updated.genres.size() == 1
                && updated.genre == updated.genres.front();
        }

        result.collectionsParity = result.collectionsZero
            && result.collectionsMultiple && result.collectionsUpdateRemoval;

        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)) {
            result.collectionsDeleteCascade = false;
        } else {
            reset(deleteItem);
            const bool deleteOk = sqlite3_bind_text(
                                    deleteItem, 1, item.id.c_str(), -1,
                                    SQLITE_TRANSIENT) == SQLITE_OK
                && sqlite3_step(deleteItem) == SQLITE_DONE;
            reset(deleteItem);
            if (!deleteOk) {
                result.message = sqlite3_errmsg(m_db);
                rollback();
                result.collectionsDeleteCascade = false;
            } else if (!exec(m_db, "COMMIT;", result.message)) {
                rollback();
                result.collectionsDeleteCascade = false;
            } else {
                MediaItem deleted;
                deleted.id = item.id;
                MediaItemSqlError readError = MediaItemSqlError::None;
                const bool childrenEmpty = readMediaItemCollections(
                    collections, deleted, readError)
                    && deleted.genres.empty() && deleted.genre.empty()
                    && deleted.imageTags.empty();
                std::int64_t remaining = 1;
                const bool rowGone = scalarInt(
                    m_db,
                    "SELECT COUNT(*) FROM media_items "
                    "WHERE id='__task09_item__';",
                    remaining, result.message) && remaining == 0;
                result.collectionsDeleteCascade = childrenEmpty && rowGone;
            }
        }

        result.success = result.collectionsParity
            && result.collectionsDeleteCascade;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = "MediaItem collection checks failed";
            }
        }
        break;
    }

    case kSeedHierarchyQueryOperation: {
        if (!exec(m_db, "BEGIN IMMEDIATE;", result.message)
            || !exec(m_db,
                     "DELETE FROM media_items WHERE id LIKE '__task10_%';",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title) VALUES"
                     "('__task10_series__', 2, 'Target Series'),"
                     "('__task10_other_series__', 2, 'Other Series');",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, series_id, "
                     "index_number) VALUES"
                     "('__task10_season_z__', 3, 'Zed', "
                     "'__task10_series__', 1),"
                     "('__task10_season_b__', 3, 'Beta', "
                     "'__task10_series__', 2),"
                     "('__task10_season_a__', 3, 'Alpha', "
                     "'__task10_series__', 1);",
                     result.message)
            || !exec(m_db,
                     "INSERT INTO media_items(id, kind, title, season_id, "
                     "series_id, index_number) VALUES"
                     "('__task10_episode_z__', 4, 'Zed Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_a__', 4, 'Alpha Episode', "
                     "'__task10_season_a__', '__task10_series__', 1),"
                     "('__task10_episode_b__', 4, 'Beta Episode', "
                     "'__task10_season_a__', '__task10_series__', 2);",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        for (int index = 0; index < 200; ++index) {
            const std::string statement =
                "INSERT INTO media_items(id, kind, title, series_id, "
                "index_number) VALUES('__task10_other_season_"
                + std::to_string(index) + "', 3, 'Other', "
                "'__task10_other_series__', " + std::to_string(index) + ");";
            if (!exec(m_db, statement.c_str(), result.message)) {
                std::string ignored;
                exec(m_db, "ROLLBACK;", ignored);
                break;
            }
            if (index == 199) {
                result.hierarchyFixture = true;
            }
        }
        if (!result.hierarchyFixture) {
            break;
        }
        if (!exec(m_db,
                  "INSERT INTO item_genres(item_id, ordinal, genre) "
                  "VALUES('__task10_season_a__', 0, 'Drama'),"
                  "('__task10_season_a__', 1, 'Mystery');",
                  result.message)
            || !exec(m_db,
                     "INSERT INTO item_image_tags(item_id, image_type, tag) "
                     "VALUES('__task10_season_a__', 'Primary', 'season-tag'),"
                     "('__task10_episode_a__', 'Primary', 'episode-tag');",
                     result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            result.hierarchyFixture = false;
            break;
        }

        auto planUsesIndex = [&](const char *sql, const char *indexName) {
            sqlite3_stmt *statement = nullptr;
            if (sqlite3_prepare_v2(m_db, sql, -1, &statement, nullptr)
                != SQLITE_OK) {
                return false;
            }
            bool found = false;
            while (sqlite3_step(statement) == SQLITE_ROW) {
                const unsigned char *detail = sqlite3_column_text(statement, 3);
                if (detail && std::string(reinterpret_cast<const char *>(detail))
                                  .find(indexName) != std::string::npos) {
                    found = true;
                }
            }
            sqlite3_finalize(statement);
            return found;
        };
        result.hierarchyIndexes = planUsesIndex(
            "EXPLAIN QUERY PLAN SELECT id FROM media_items "
            "WHERE series_id='__task10_series__' AND kind=3 "
            "ORDER BY index_number, title, id;",
            "idx_media_series_kind_order")
            && planUsesIndex(
                "EXPLAIN QUERY PLAN SELECT id FROM media_items "
                "WHERE season_id='__task10_season_a__' AND kind=4 "
                "ORDER BY index_number, title, id;",
                "idx_media_season_order");
        if (!exec(m_db, "COMMIT;", result.message)) {
            std::string ignored;
            exec(m_db, "ROLLBACK;", ignored);
            break;
        }
        result.success = result.hierarchyFixture && result.hierarchyIndexes;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "hierarchy query index plan check failed";
        }
        break;
    }

    case kClearHierarchyQueryOperation:
        result.success = exec(
            m_db, "DELETE FROM media_items WHERE id LIKE '__task10_%';",
            result.message);
        break;

    case kWriteSentinelOperation: {
        if (!exec(m_db,
                  "CREATE TABLE IF NOT EXISTS catalog_task05_sentinel(value TEXT NOT NULL)",
                  result.message)
            || !exec(m_db, "DELETE FROM catalog_task05_sentinel",
                     result.message)) {
            result.error = CatalogDbErrorCategory::SqliteError;
            break;
        }
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_insert",
                          "INSERT INTO catalog_task05_sentinel(value) VALUES(?1)",
                          statement)) {
            break;
        }
        sqlite3_bind_text(statement, 1, command->value.c_str(), -1,
                          SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement);
        reset(statement);
        result.success = rc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kWriteSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_write",
                "INSERT OR REPLACE INTO media_items(id, kind, title) "
                "VALUES('__task06_marker__', 1, ?1)",
                statement)) {
            break;
        }
        const int bindRc = sqlite3_bind_text(statement, 1,
                                             command->value.c_str(), -1,
                                             SQLITE_TRANSIENT);
        const int stepRc = bindRc == SQLITE_OK ? sqlite3_step(statement)
                                                : bindRc;
        reset(statement);
        result.success = stepRc == SQLITE_DONE;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        break;
    }

    case kReadSchemaMarkerOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed(
                "schema_marker_read",
                "SELECT title FROM media_items "
                "WHERE id='__task06_marker__'",
                statement)) {
            break;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(statement, 0);
            result.sentinel = value ? reinterpret_cast<const char *>(value) : "";
            result.success = true;
        } else {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        break;
    }

    case kSetSchemaMetadataOperation: {
        const std::size_t separator = command->value.find(':');
        if (separator == std::string::npos) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            result.message = "invalid schema metadata fixture";
            break;
        }
        const std::string applicationId = command->value.substr(0, separator);
        const std::string userVersion = command->value.substr(separator + 1);
        const std::string applicationIdPragma =
            "PRAGMA application_id = " + applicationId + ";";
        const std::string userVersionPragma =
            "PRAGMA user_version = " + userVersion + ";";
        result.success = exec(m_db, applicationIdPragma.c_str(), result.message)
            && exec(m_db, userVersionPragma.c_str(), result.message);
        if (!result.success) {
            result.error = CatalogDbErrorCategory::SqliteError;
        }
        break;
    }

    case kMigrationRollbackOperation: {
        finalizeStatements();
        std::string migrationError;
        const bool began = exec(m_db, "BEGIN IMMEDIATE;", migrationError);
        const int failedMigration = began
            ? execResult(m_db,
                         "CREATE TABLE catalog_task07_rollback_probe(value TEXT);"
                         "INSERT INTO catalog_task07_missing(value) VALUES(1);",
                         migrationError)
            : SQLITE_ERROR;
        std::string rollbackError;
        const bool rolledBack = began
            && exec(m_db, "ROLLBACK;", rollbackError);
        std::int64_t probeCount = 1;
        const bool inspected = scalarInt(
            m_db,
            "SELECT COUNT(*) FROM sqlite_master WHERE name="
            "'catalog_task07_rollback_probe';",
            probeCount, result.message);
        sqlite3_stmt *probe = nullptr;
        const int prepareRc = sqlite3_prepare_v2(m_db, "SELECT 1", -1,
                                                 &probe, nullptr);
        if (prepareRc == SQLITE_OK && probe) {
            sqlite3_step(probe);
            sqlite3_reset(probe);
            sqlite3_clear_bindings(probe);
            m_statements.emplace("migration_probe", probe);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_preparedStatementCount = m_statements.size();
        }
        result.statementReused = prepareRc == SQLITE_OK && probe != nullptr;
        result.success = failedMigration != SQLITE_OK && rolledBack
            && inspected && probeCount == 0 && result.statementReused;
        if (!result.success) {
            result.error = CatalogDbErrorCategory::ConfigurationFailed;
            if (result.message.empty()) {
                result.message = migrationError.empty() ? rollbackError
                                                        : migrationError;
            }
        }
        break;
    }

    case kReadSentinelOperation: {
        sqlite3_stmt *statement = nullptr;
        if (!prepareNamed("sentinel_read",
                          "SELECT value FROM catalog_task05_sentinel LIMIT 1",
                          statement)) {
            break;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            const unsigned char *value = sqlite3_column_text(statement, 0);
            result.sentinel = value ? reinterpret_cast<const char *>(value) : "";
            result.success = true;
        } else {
            result.error = CatalogDbErrorCategory::SqliteError;
            result.message = sqlite3_errmsg(m_db);
        }
        reset(statement);
        break;
    }

    default:
        result.error = CatalogDbErrorCategory::SqliteError;
        result.message = "unknown CatalogDb test operation";
        break;
    }

    command->result.set_value(std::move(result));
}

CatalogDbTestResult CatalogDb::runTestCommand(unsigned char operation,
                                              const std::string &value)
{
    auto command = std::make_shared<TestCommand>();
    command->operation = operation;
    command->value = value;
    std::future<CatalogDbTestResult> result = command->result.get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopping) {
            CatalogDbTestResult stopped;
            stopped.error = CatalogDbErrorCategory::ScopeNotReady;
            stopped.message = "CatalogDb is stopping";
            return stopped;
        }
        if (m_testCommands.size() >= kMaxPendingJobs) {
            CatalogDbTestResult full;
            full.error = CatalogDbErrorCategory::OpenFailed;
            full.message = "CatalogDb test command queue is full";
            return full;
        }
        m_testCommands.push_back(command);
    }
    m_wake.notify_one();
    return result.get();
}

} // namespace miyoofin
