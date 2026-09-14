#pragma once

#include "CatalogDb.hpp"
#include "CatalogCompatibility.hpp"
#include "../data/MediaItem.hpp"
#include "MediaItemSql.hpp"
#include "CatalogDbSchema.hpp"
#include "../data/CatalogPrimitives.hpp"
#include "../diagnostics/UiDiagnostics.hpp"
#include "../diagnostics/PerformanceTelemetry.hpp"
#include "../diagnostics/TelemetryClock.hpp"
#include "../../vendor/sqlite/sqlite3.h"
#include <algorithm>
#include <cassert>
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

struct CatalogDb::QueryCommand {
    enum class Kind : unsigned char {
        Seasons,
        Episodes,
        MediaItemsByIds,
        DeleteMediaItemsByIds,
    };

    Kind kind;
    std::string parentId;
    std::vector<std::string> itemIds;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbHierarchyResult> result;
};

struct CatalogDb::LibraryReadCommand {
    CatalogDbJobMetadata metadata;
    std::promise<CatalogCompatibilityReadResult> result;
};

struct CatalogDb::MediaPageCommand {
    std::string type; int letter = -1; std::size_t limit = 0;
    CatalogDbMediaPageFilter filter = CatalogDbMediaPageFilter::Supported;
    CatalogDbPageCursor after; CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbMediaPageResult> result;
};

struct CatalogDb::ReconcileCommand {
    std::vector<MediaItem> series;
    bool authoritative = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterRows = -1;
    std::promise<CatalogDbReconcileResult> result;
};

struct CatalogDb::SyncStateCommand {
    bool write = false;
    bool legacyAvailable = false;
    std::int64_t legacyLastSuccessfulMs = 0;
    std::int64_t legacyLastReconcileMs = 0;
    std::int64_t lastSuccessfulMs = 0;
    std::int64_t lastReconcileMs = 0;
    std::uint64_t committedGeneration = 0;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    std::promise<CatalogDbSyncState> result;
};

struct CatalogDb::LibrarySeedCommand {
    CatalogCompatibilitySeedRequest request;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterWrites = -1;
    std::promise<CatalogCompatibilitySeedResult> result;
};

struct CatalogDb::HierarchyWriteCommand {
    MediaItem series;
    std::vector<MediaItem> seasons;
    std::map<std::string, std::vector<MediaItem>> episodesBySeason;
    std::uint64_t generation = 0;
    std::int64_t refreshMs = 0;
    bool complete = true;
    bool seasonScoped = false;
    CatalogDbJobMetadata metadata;
    std::uint64_t enqueuedMonotonicUs = 0;
    int failAfterRows = -1;
    int cancelAfterRows = -1;
    std::promise<CatalogDbHierarchyWriteResult> result;
};

struct CatalogDb::MediaPageUpsertCommand {
    CatalogDbMediaPageWrite page;
    CatalogDbJobMetadata metadata;
    int failAfterRows = -1;
    std::promise<CatalogDbMediaPageUpsertResult> result;
};

struct CatalogDb::TopLevelSyncCommand {
    bool begin = false;
    bool finalize = false;
    std::uint64_t generation = 0;
    CatalogDbJobMetadata metadata;
    std::promise<CatalogDbTopLevelSyncResult> result;
};

namespace catalog_db_internal {


static inline const char *migrationDecisionName(CatalogDbMigrationDecision value)
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

static inline const char *scopeStatusName(CatalogDbScopeStatus value)
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

static inline const char *errorCategoryName(CatalogDbErrorCategory value)
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

static inline const char *openStateName(CatalogDbOpenState value)
{
    switch (value) {
    case CatalogDbOpenState::NotAttempted: return "not_attempted";
    case CatalogDbOpenState::CreatedV1: return "created_v1";
    case CatalogDbOpenState::SupportedV1: return "supported_v1";
    case CatalogDbOpenState::CreatedV2: return "created_v2";
    case CatalogDbOpenState::SupportedV2: return "supported_v2";
    case CatalogDbOpenState::CreatedV3: return "created_v3";
    case CatalogDbOpenState::SupportedV3: return "supported_v3";
    case CatalogDbOpenState::WrongApplicationId: return "wrong_application_id";
    case CatalogDbOpenState::UnsupportedVersion: return "unsupported_version";
    case CatalogDbOpenState::CorruptOrIo: return "corrupt_or_io";
    }
    return "unknown";
}

static inline std::string scopeDirectory(const std::string &scopeKey)
{
    return std::string("cache/library/") + scopeKey;
}

static inline void catalogDiagnostic(const std::string &line)
{
    uiDiagnostics().log(std::string("[CatalogDb] ") + line);
}

static inline void catalogFinalDiagnostic(bool ready, CatalogDbScopeStatus status,
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
constexpr unsigned char kMediaPageQueryPlanOperation = 15;
constexpr std::size_t kMaxHierarchyQueryRows = 128;
constexpr std::size_t kMaxMetadataByIdRows = 64;
constexpr std::size_t kMaxReconcileSeries = 4096;

static inline bool validScopeIdentity(const std::string &serverUrl, const std::string &userId)
{
    const std::string normalizedUrl = catalog::normalizeIdentityUrl(serverUrl);
    const std::size_t schemeEnd = normalizedUrl.find("://");
    return schemeEnd != std::string::npos
        && schemeEnd + 3 < normalizedUrl.size()
        && userId.find_first_not_of(" \t\r\n") != std::string::npos;
}

static inline bool makeDirectories(const std::string &path)
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

static inline std::string catalogPath(const std::string &scopeKey)
{
    return catalog::catalogPath("cache", scopeKey);
}

static inline std::string migratingPath(const std::string &scopeKey)
{
    return catalog::migratingCatalogPath("cache", scopeKey);
}

struct MigrationPathPresence {
    bool present = false;
    bool error = false;
};

static inline MigrationPathPresence inspectMigrationPath(const std::string &path)
{
    struct stat information {
    };
    if (::stat(path.c_str(), &information) == 0) {
        return {true, false};
    }
    return {false, errno != ENOENT};
}

static inline CatalogDbMigrationState inspectMigrationState(const std::string &scopeKey)
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

static inline uint64_t telemetryNowIfEnabled() noexcept
{
    return performanceTelemetry().enabledFast()
        ? TelemetryClock::monotonicUs() : 0;
}

static inline bool mediaItemRowExists(sqlite3 *db, const std::string &id, bool &exists,
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

static inline int scalarCallback(void *context, int columnCount, char **values, char **)
{
    if (columnCount > 0 && values[0]) {
        static_cast<ScalarValue *>(context)->value = values[0];
    }
    return 0;
}

static inline bool scalar(sqlite3 *db, const char *sql, std::string &value,
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

static inline bool exec(sqlite3 *db, const char *sql, std::string &error)
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

static inline bool scalarInt(sqlite3 *db, const char *sql, std::int64_t &value,
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

static inline int execResult(sqlite3 *db, const char *sql, std::string &error)
{
    char *sqliteError = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &sqliteError);
    if (rc != SQLITE_OK) {
        error = sqliteError ? sqliteError : sqlite3_errmsg(db);
        sqlite3_free(sqliteError);
    }
    return rc;
}

static inline bool collectObjects(sqlite3 *db, std::set<std::string> &objects,
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

static inline bool ensureSchema(sqlite3 *db, CatalogDbOpenState &openState,
                  bool &needsBackfill, std::string &error)
{
    needsBackfill = false;
    std::int64_t applicationId = 0;
    std::int64_t userVersion = 0;
    if (!scalarInt(db, "PRAGMA application_id;", applicationId, error)
        || !scalarInt(db, "PRAGMA user_version;", userVersion, error)) {
        openState = CatalogDbOpenState::CorruptOrIo;
        return false;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 3) {
        openState = CatalogDbOpenState::SupportedV3;
        return true;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 2) {
        needsBackfill = true;
        if (!exec(db, "BEGIN IMMEDIATE;", error)) return false;
        const char *const migration[] = {
            "ALTER TABLE media_items ADD COLUMN organizational_sort_key TEXT NOT NULL DEFAULT '';",
            "CREATE INDEX idx_media_movie_sort ON media_items(kind, organizational_sort_key, title, id);",
            "CREATE INDEX idx_media_show_sort ON media_items(kind, organizational_sort_key, title, id);",
        };
        for (const char *statement : migration) {
            if (!exec(db, statement, error)) { std::string ignored; exec(db, "ROLLBACK;", ignored); return false; }
        }
        if (!exec(db, "PRAGMA user_version = 3;", error) || !exec(db, "COMMIT;", error)) { std::string ignored; exec(db, "ROLLBACK;", ignored); return false; }
        openState = CatalogDbOpenState::SupportedV3;
        return true;
    }
    if (applicationId == kCatalogApplicationId && userVersion == 1) {
        needsBackfill = true;
        if (!exec(db, "BEGIN IMMEDIATE;", error))
            return false;
        const char *const migration[] = {
            "CREATE TABLE library_views ("
            "id TEXT PRIMARY KEY NOT NULL,"
            "name TEXT NOT NULL DEFAULT '',"
            "collection_type TEXT NOT NULL DEFAULT '',"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0)"
            ");",
            "CREATE TABLE library_membership ("
            "view_id TEXT NOT NULL, item_id TEXT NOT NULL,"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
            "PRIMARY KEY(view_id, item_id),"
            "FOREIGN KEY(view_id) REFERENCES library_views(id) ON DELETE CASCADE,"
            "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
            ");",
            "CREATE TABLE home_items ("
            "row_kind TEXT NOT NULL, item_id TEXT NOT NULL,"
            "ordinal INTEGER NOT NULL CHECK(ordinal >= 0),"
            "PRIMARY KEY(row_kind, item_id),"
            "FOREIGN KEY(item_id) REFERENCES media_items(id) ON DELETE CASCADE"
            ");",
            "CREATE INDEX idx_library_views_ordinal ON library_views(ordinal, id);",
            "CREATE INDEX idx_library_membership_view_order ON library_membership(view_id, ordinal, item_id);",
            "CREATE INDEX idx_library_membership_item ON library_membership(item_id, view_id);",
            "CREATE INDEX idx_home_items_row_order ON home_items(row_kind, ordinal, item_id);",
            "ALTER TABLE media_items ADD COLUMN organizational_sort_key TEXT NOT NULL DEFAULT '';",
            "CREATE INDEX idx_media_movie_sort ON media_items(kind, organizational_sort_key, title, id);",
            "CREATE INDEX idx_media_show_sort ON media_items(kind, organizational_sort_key, title, id);",
        };
        for (const char *statement : migration) {
            if (!exec(db, statement, error)) {
                std::string ignored;
                exec(db, "ROLLBACK;", ignored);
                return false;
            }
        }
        if (!exec(db, "PRAGMA user_version = 3;", error)
            || !exec(db, "COMMIT;", error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
        openState = CatalogDbOpenState::SupportedV2;
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
    // A newly-created schema has no pre-existing rows to canonicalize.
    for (const char *statement : kCatalogSchemaStatements) {
        if (!exec(db, statement, error)) {
            std::string ignored;
            exec(db, "ROLLBACK;", ignored);
            return false;
        }
    }
    const std::string applicationIdPragma =
        "PRAGMA application_id = " + std::to_string(kCatalogApplicationId) + ";";
    if (!exec(db, applicationIdPragma.c_str(), error)
        || !exec(db, "PRAGMA user_version = 3;", error)
        || !exec(db, "COMMIT;", error)) {
        std::string ignored;
        exec(db, "ROLLBACK;", ignored);
        return false;
    }
    openState = CatalogDbOpenState::CreatedV3;
    return true;
}

static inline bool backfillOrganizationalSortKeys(sqlite3 *db, std::string &error)
{
    sqlite3_stmt *read = nullptr;
    sqlite3_stmt *write = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT id, title FROM media_items", -1, &read, nullptr) != SQLITE_OK
        || sqlite3_prepare_v2(db, "UPDATE media_items SET organizational_sort_key=?1 WHERE id=?2", -1, &write, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db); sqlite3_finalize(read); sqlite3_finalize(write); return false;
    }
    while (sqlite3_step(read) == SQLITE_ROW) {
        const char *id = reinterpret_cast<const char *>(sqlite3_column_text(read, 0));
        const char *title = reinterpret_cast<const char *>(sqlite3_column_text(read, 1));
        sqlite3_reset(write); sqlite3_clear_bindings(write);
        if (!id || sqlite3_bind_text(write, 1, catalog::organizationalSortKey(title ? title : "").c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK
            || sqlite3_bind_text(write, 2, id, -1, SQLITE_TRANSIENT) != SQLITE_OK
            || sqlite3_step(write) != SQLITE_DONE) { error = sqlite3_errmsg(db); sqlite3_finalize(read); sqlite3_finalize(write); return false; }
    }
    sqlite3_finalize(read); sqlite3_finalize(write); return true;
}

static inline bool maintainOrganizationalSortKey(sqlite3 *db, const MediaItem &item,
                                   std::string &error)
{
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, "UPDATE media_items SET organizational_sort_key=?1 WHERE id=?2", -1, &statement, nullptr) != SQLITE_OK) { error = sqlite3_errmsg(db); return false; }
    const std::string key = catalog::organizationalSortKey(item.title);
    const bool ok = sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
        && sqlite3_bind_text(statement, 2, item.id.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK
        && sqlite3_step(statement) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(db);
    sqlite3_finalize(statement);
    return ok;
}

static inline bool schemaConstraints(sqlite3 *db, bool &foreignKeyCascade,
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

static inline bool sameMediaItemScalars(const MediaItem &expected, const MediaItem &actual)
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

static inline bool validateHierarchyInput(
    const MediaItem &series, const std::vector<MediaItem> &seasons,
    const std::map<std::string, std::vector<MediaItem>> &episodesBySeason,
    std::string &error)
{
    if (series.id.empty() || !series.seriesId.empty()
        || !series.seasonId.empty()) {
        error = "invalid series hierarchy root";
        return false;
    }
    if (series.type == "movie") {
        if (!seasons.empty() || !episodesBySeason.empty()) {
            error = "movie hierarchy root cannot contain children";
            return false;
        }
        return true;
    }
    if (series.type != "show") {
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

static inline bool validateReconcileInput(const std::vector<MediaItem> &series,
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


} // namespace catalog_db_internal
} // namespace miyoofin
